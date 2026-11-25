/* In-process sampling profiler for the static image (host and device).
 *
 *   PORT_PROF="<start seconds>:<duration seconds>[:<period microseconds>]"
 *
 * After <start> seconds a SIGPROF timer (process CPU time, default 1000 us)
 * samples the program counter of whichever thread is burning CPU; after
 * <duration> the samples are attributed with dladdr() and written to stderr
 * (logcat on the device) as PORT_PROF lines:
 *   - CPU share per thread (kernel thread name),
 *   - CPU share per loaded module,
 *   - the top symbols (lifted functions are exported as sub_XXXXXXXX),
 *   - machine-lock statistics for the window (runtime/static_threads.c).
 * Nothing is measured unless PORT_PROF is set; the handler only stores two
 * words per sample. It measures where CPU time goes, not wall time: a thread
 * that waits (for the machine lock, the GPU, vsync) does not appear, which is
 * why the lock statistics and the per-thread shares are printed with it. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROF_MAX_SAMPLES (1u << 18)

typedef struct { uintptr_t pc; uint32_t tid; } ProfSample;
static ProfSample *prof_samples;
static atomic_uint prof_count;
static atomic_int prof_on;

/* Wall-clock activity: every thread that runs guest code publishes what it
 * is doing right now -- the Win32/D3D9 bridge it is in, or "machine wait"
 * while it waits for the guest CPU. The sampler reads the table every
 * millisecond, so it also sees time spent blocked (which the CPU profile
 * cannot). port_act_enter() returns the previous activity for nesting. */
#define ACT_SLOTS 96
typedef struct { atomic_int tid; const char *_Atomic what; atomic_uint esp; } ActSlot;
static ActSlot act_slots[ACT_SLOTS];
static atomic_int act_used;
static __thread int act_index = -1;
const char *port_act_enter(const char *what) {
    if (act_index < 0) {
        int i = atomic_fetch_add(&act_used, 1);
        if (i >= ACT_SLOTS) { act_index = ACT_SLOTS; return NULL; }
        atomic_store(&act_slots[i].tid, (int)syscall(SYS_gettid));
        act_index = i;
    }
    if (act_index >= ACT_SLOTS) return NULL;
    return atomic_exchange_explicit(&act_slots[act_index].what, what, memory_order_relaxed);
}
/* The guest stack pointer of the bridge call in progress (0 = unknown). */
void port_act_esp(uint32_t esp) {
    if (act_index >= 0 && act_index < ACT_SLOTS) atomic_store_explicit(&act_slots[act_index].esp, esp, memory_order_relaxed);
}
void port_act_leave(const char *previous) {
    if (act_index >= 0 && act_index < ACT_SLOTS)
        atomic_store_explicit(&act_slots[act_index].what, previous, memory_order_relaxed);
}

/* Bridge statistics while a window is open: calls and wall time per bridge,
 * and for waits the (caller, timeout) pairs with their outcome. */
static atomic_int stats_on;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { const char *name; unsigned calls; uint64_t ns; } BridgeStat;
typedef struct { uint32_t caller, timeout; unsigned calls, timeouts; uint64_t ns; } WaitStat;
static BridgeStat bridge_stats[256];
static WaitStat wait_stats[128];
int port_stats_active(void) { return atomic_load_explicit(&stats_on, memory_order_relaxed); }
void port_bridge_stat(const char *name, uint64_t ns) {
    pthread_mutex_lock(&stats_lock);
    for (unsigned i = 0; i < 256; i++) {
        if (bridge_stats[i].name == name || !bridge_stats[i].name) {
            bridge_stats[i].name = name; bridge_stats[i].calls++; bridge_stats[i].ns += ns; break;
        }
    }
    pthread_mutex_unlock(&stats_lock);
}
void port_wait_stat(uint32_t caller, uint32_t timeout, int timed_out, uint64_t ns) {
    pthread_mutex_lock(&stats_lock);
    for (unsigned i = 0; i < 128; i++) {
        WaitStat *w = &wait_stats[i];
        if ((w->caller == caller && w->timeout == timeout) || !w->calls) {
            w->caller = caller; w->timeout = timeout; w->calls++; w->timeouts += timed_out ? 1u : 0u; w->ns += ns; break;
        }
    }
    pthread_mutex_unlock(&stats_lock);
}
static void stats_report(double seconds) {
    pthread_mutex_lock(&stats_lock);
    for (unsigned a = 0; a < 256 && bridge_stats[a].name; a++)
        for (unsigned b = a + 1; b < 256 && bridge_stats[b].name; b++)
            if (bridge_stats[b].calls > bridge_stats[a].calls) { BridgeStat t = bridge_stats[a]; bridge_stats[a] = bridge_stats[b]; bridge_stats[b] = t; }
    for (unsigned a = 0; a < 256 && bridge_stats[a].name && a < 40; a++)
        fprintf(stderr, "PORT_PROF api %-28s %8.1f calls/s %8.2f ms/s\n", bridge_stats[a].name,
                bridge_stats[a].calls / seconds, bridge_stats[a].ns / 1e6 / seconds);
    for (unsigned a = 0; a < 128 && wait_stats[a].calls; a++)
        for (unsigned b = a + 1; b < 128 && wait_stats[b].calls; b++)
            if (wait_stats[b].ns > wait_stats[a].ns) { WaitStat t = wait_stats[a]; wait_stats[a] = wait_stats[b]; wait_stats[b] = t; }
    for (unsigned a = 0; a < 128 && wait_stats[a].calls && a < 20; a++)
        fprintf(stderr, "PORT_PROF wait from 0x%08X timeout %u ms: %.1f/s, %u timed out, avg %.2f ms\n",
                wait_stats[a].caller, wait_stats[a].timeout, wait_stats[a].calls / seconds, wait_stats[a].timeouts,
                wait_stats[a].ns / 1e6 / wait_stats[a].calls);
    memset(bridge_stats, 0, sizeof bridge_stats);
    memset(wait_stats, 0, sizeof wait_stats);
    pthread_mutex_unlock(&stats_lock);
}

/* runtime/static_threads.c */
void st_machine_stats(uint64_t *acquired, uint64_t *contended, uint64_t *wait_ns);
void st_machine_report(void);
void st_machine_report_reset(void);

static void prof_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    if (!atomic_load_explicit(&prof_on, memory_order_relaxed)) return;
    unsigned index = atomic_fetch_add_explicit(&prof_count, 1, memory_order_relaxed);
    if (index >= PROF_MAX_SAMPLES) return;
    ucontext_t *uc = (ucontext_t *)context;
#if defined(__aarch64__)
    prof_samples[index].pc = (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__x86_64__)
    prof_samples[index].pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    prof_samples[index].pc = 0;
#endif
    prof_samples[index].tid = (uint32_t)syscall(SYS_gettid);
}

static double prof_start_after, prof_duration;
static int prof_auto_mode;
/* PORT_PROF_TRIGGER=<path>: in auto mode, a file appearing there starts a
 * 10 s window right away (and is removed). */
static char prof_trigger_path[512];
static long prof_period_us = 1000;

typedef struct { char name[96]; unsigned count; } ProfBucket;

static void bucket_add(ProfBucket *buckets, unsigned *used, unsigned capacity, const char *name) {
    for (unsigned i = 0; i < *used; i++)
        if (!strcmp(buckets[i].name, name)) { buckets[i].count++; return; }
    if (*used >= capacity) { buckets[capacity - 1].count++; return; }   /* overflow bucket */
    snprintf(buckets[*used].name, sizeof buckets[*used].name, "%s", name);
    buckets[(*used)++].count = 1;
}
static int bucket_cmp(const void *a, const void *b) {
    unsigned x = ((const ProfBucket *)a)->count, y = ((const ProfBucket *)b)->count;
    return x < y ? 1 : (x > y ? -1 : 0);
}
static void bucket_print(const char *title, ProfBucket *buckets, unsigned used, unsigned total, unsigned top) {
    qsort(buckets, used, sizeof *buckets, bucket_cmp);
    for (unsigned i = 0; i < used && i < top; i++)
        fprintf(stderr, "PORT_PROF %s %5.1f%% %6u  %s\n", title, 100.0 * buckets[i].count / (total ? total : 1),
                buckets[i].count, buckets[i].name);
}

static void prof_report(double seconds, uint64_t lock0[3]) {
    unsigned total = atomic_load(&prof_count);
    if (total > PROF_MAX_SAMPLES) total = PROF_MAX_SAMPLES;
    enum { SYMS = 6000, MODS = 64, THREADS = 128 };
    ProfBucket *syms = calloc(SYMS, sizeof *syms), *mods = calloc(MODS, sizeof *mods), *threads = calloc(THREADS, sizeof *threads);
    unsigned nsym = 0, nmod = 0, nthread = 0;
    if (!syms || !mods || !threads) return;
    for (unsigned i = 0; i < total; i++) {
        Dl_info info;
        char name[96], module[96] = "?";
        memset(&info, 0, sizeof info);
        if (dladdr((void *)prof_samples[i].pc, &info) && info.dli_fname) {
            const char *base = strrchr(info.dli_fname, '/');
            snprintf(module, sizeof module, "%s", base ? base + 1 : info.dli_fname);
        }
        if (info.dli_sname) snprintf(name, sizeof name, "%.60s [%.28s]", info.dli_sname, module);
        else snprintf(name, sizeof name, "+0x%lx [%.40s]",
                      (unsigned long)((prof_samples[i].pc - (uintptr_t)info.dli_fbase) & ~0x3fUL), module);
        bucket_add(syms, &nsym, SYMS, name);
        bucket_add(mods, &nmod, MODS, module);
        char comm[40] = "?", path[64], thread[96];
        snprintf(path, sizeof path, "/proc/self/task/%u/comm", prof_samples[i].tid);
        FILE *f = fopen(path, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        snprintf(thread, sizeof thread, "%s (tid %u)", comm, prof_samples[i].tid);
        bucket_add(threads, &nthread, THREADS, thread);
    }
    uint64_t lock1[3];
    st_machine_stats(&lock1[0], &lock1[1], &lock1[2]);
    fprintf(stderr, "PORT_PROF window %.1fs samples=%u (= %.2f cores busy) machine-lock: acquired=%llu contended=%llu waited=%.2fs\n",
            seconds, total, total * (prof_period_us / 1e6) / seconds, (unsigned long long)(lock1[0] - lock0[0]),
            (unsigned long long)(lock1[1] - lock0[1]), (double)(lock1[2] - lock0[2]) / 1e9);
    bucket_print("thread", threads, nthread, total, 24);
    bucket_print("module", mods, nmod, total, 16);
    bucket_print("symbol", syms, nsym, total, 40);
    /* The same per thread, for the busiest ones (threads[] is sorted now). */
    for (unsigned t = 0; t < nthread && t < 8; t++) {
        unsigned tid = 0, count = 0;
        const char *open = strstr(threads[t].name, "(tid ");
        if (open) tid = (unsigned)strtoul(open + 5, NULL, 10);
        nsym = 0;
        memset(syms, 0, SYMS * sizeof *syms);
        for (unsigned i = 0; i < total; i++) {
            if (prof_samples[i].tid != tid) continue;
            Dl_info info;
            char name[96], module[96] = "?";
            memset(&info, 0, sizeof info);
            if (dladdr((void *)prof_samples[i].pc, &info) && info.dli_fname) {
                const char *base = strrchr(info.dli_fname, '/');
                snprintf(module, sizeof module, "%s", base ? base + 1 : info.dli_fname);
            }
            if (info.dli_sname) snprintf(name, sizeof name, "%.60s [%.28s]", info.dli_sname, module);
            else snprintf(name, sizeof name, "+0x%lx [%.40s]",
                          (unsigned long)((prof_samples[i].pc - (uintptr_t)info.dli_fbase) & ~0x3fUL), module);
            bucket_add(syms, &nsym, SYMS, name);
            count++;
        }
        char title[64];
        snprintf(title, sizeof title, "in[%.24s]", threads[t].name);
        bucket_print(title, syms, nsym, count, 14);
    }
    st_machine_report();
    fprintf(stderr, "PORT_PROF end\n");
    free(syms); free(mods); free(threads);
}

/* Sleeps the whole time even when signals interrupt it. */
static void prof_sleep(double seconds) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += (time_t)seconds;
    end.tv_nsec += (long)((seconds - (double)(time_t)seconds) * 1e9);
    if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, NULL) != 0) { }
}

/* Wall-clock sampler: one thread, 1 ms period, for `seconds`. */
typedef struct { int tid; const char *what; uint32_t ret[3]; unsigned count; } ActBucket;
uint8_t *port_flat_base(void);                  /* runtime/static_entry.c */
static void act_sample(double seconds) {
    enum { MAX = 512 };
    ActBucket *buckets = calloc(MAX, sizeof *buckets);
    unsigned used = 0, ticks = 0;
    if (!buckets) return;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    long total = (long)(seconds * 1000);
    for (long n = 0; n < total; n++) {
        next.tv_nsec += 1000000;
        if (next.tv_nsec >= 1000000000L) { next.tv_sec++; next.tv_nsec -= 1000000000L; }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) != 0) { }
        ticks++;
        int slots = atomic_load(&act_used);
        if (slots > ACT_SLOTS) slots = ACT_SLOTS;
        for (int i = 0; i < slots; i++) {
            int tid = atomic_load(&act_slots[i].tid);
            const char *what = atomic_load_explicit(&act_slots[i].what, memory_order_relaxed);
            uint32_t ret[3] = { 0, 0, 0 };
            if (!what) what = "(guest code)";
            else {
                /* Return addresses into the image's code above the bridge's
                 * stack pointer: which guest code made the call. */
                uint32_t esp = atomic_load_explicit(&act_slots[i].esp, memory_order_relaxed);
                uint8_t *flat = port_flat_base();
                for (uint32_t off = 0, n_ret = 0; flat && esp && off < 512 && n_ret < 3; off += 4) {
                    uint32_t v; memcpy(&v, flat + (uint32_t)(esp + off), 4);
                    if (v >= 0x00401000u && v < 0x00A2F000u) ret[n_ret++] = v;
                }
            }
            unsigned k = 0;
            while (k < used && !(buckets[k].tid == tid && buckets[k].what == what && !memcmp(buckets[k].ret, ret, sizeof ret))) k++;
            if (k == used) { if (used == MAX) continue; buckets[used].tid = tid; buckets[used].what = what; memcpy(buckets[used].ret, ret, sizeof ret); used++; }
            buckets[k].count++;
        }
    }
    /* Report per thread, sorted by count. */
    for (unsigned a = 0; a < used; a++)
        for (unsigned b = a + 1; b < used; b++)
            if (buckets[b].tid < buckets[a].tid || (buckets[b].tid == buckets[a].tid && buckets[b].count > buckets[a].count)) {
                ActBucket t = buckets[a]; buckets[a] = buckets[b]; buckets[b] = t;
            }
    fprintf(stderr, "PORT_PROF wall-clock activity over %u ms (share of time per thread):\n", ticks);
    int shown_for = -1, lines = 0;
    for (unsigned k = 0; k < used; k++) {
        if (buckets[k].tid != shown_for) { shown_for = buckets[k].tid; lines = 0; }
        if (lines++ >= 12 || buckets[k].count * 200 < ticks) continue;  /* top 12, >= 0.5 % */
        char path[64], comm[32] = "?";
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", buckets[k].tid);
        FILE *f = fopen(path, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        fprintf(stderr, "PORT_PROF wall %-16s tid %-6d %5.1f%%  %s <- 0x%08X <- 0x%08X <- 0x%08X\n", comm, buckets[k].tid,
                100.0 * buckets[k].count / ticks, buckets[k].what, buckets[k].ret[0], buckets[k].ret[1], buckets[k].ret[2]);
    }
    free(buckets);
}
static void *act_thread(void *arg) {
    double seconds = *(double *)arg;
    free(arg);
    pthread_setname_np(pthread_self(), "port-prof-wall");
    act_sample(seconds);
    return NULL;
}

/* One sampling window of `seconds`, then the report. */
static void prof_window(double seconds, const char *label) {
    uint64_t lock0[3];
    if (label) fprintf(stderr, "PORT_PROF trigger: %s\n", label);
    prof_duration = seconds;
    st_machine_stats(&lock0[0], &lock0[1], &lock0[2]);
    st_machine_report_reset();
    atomic_store(&prof_count, 0);
    atomic_store(&prof_on, 1);
    struct itimerval timer = { { 0, prof_period_us }, { 0, prof_period_us } };
    setitimer(ITIMER_PROF, &timer, NULL);
    fprintf(stderr, "PORT_PROF sampling for %.0fs every %ld us\n", prof_duration, prof_period_us);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t wall;
    double *wall_seconds = malloc(sizeof *wall_seconds);
    int wall_started = 0;
    if (wall_seconds) {
        *wall_seconds = prof_duration;
        wall_started = pthread_create(&wall, NULL, act_thread, wall_seconds) == 0;
        if (!wall_started) free(wall_seconds);
    }
    atomic_store(&stats_on, 1);
    prof_sleep(prof_duration);
    atomic_store(&stats_on, 0);
    atomic_store(&prof_on, 0);
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_PROF, &off, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    prof_report((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9, lock0);
    if (wall_started) pthread_join(wall, NULL);
    stats_report((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) * 1e-9);
}

/* PORT_PROF=auto: watch the presented frame rate and sample by itself --
 * 15 s whenever it falls below 25 fps for 5 s (up to 3 times, a minute apart),
 * and once for comparison after 20 s at 55 fps or more. For problems that
 * only appear at some point of a session on the device. */
uint64_t d3d9t_frames(void);                    /* runtime/d3d9_thunk.cpp */
static void prof_auto(void) {
    uint64_t history[4] = { 0, 0, 0, 0 };
    int low_run = 0, high_run = 0, low_reports = 0, high_reports = 0;
    double last_report = -1e9;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    fprintf(stderr, "PORT_PROF auto: watching the frame rate\n");
    for (;;) {
        prof_sleep(1.0);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double up = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) * 1e-9;
        if (prof_trigger_path[0] && access(prof_trigger_path, F_OK) == 0) {
            unlink(prof_trigger_path);
            prof_window(10, "manual (trigger file)");
            continue;
        }
        /* "<trigger>.stats": only the bridge counters for 5 s -- no SIGPROF,
         * no wall-clock sampler. Sampling every thread 1000 times a second
         * is enough of a disturbance to tip the game into its slow state, so
         * the fast state can only be observed this way. */
        if (prof_trigger_path[0]) {
            char stats_path[600];
            snprintf(stats_path, sizeof stats_path, "%s.stats", prof_trigger_path);
            if (access(stats_path, F_OK) == 0) {
                unlink(stats_path);
                uint64_t f0 = d3d9t_frames();
                struct timespec s0, s1;
                clock_gettime(CLOCK_MONOTONIC, &s0);
                atomic_store(&stats_on, 1);
                prof_sleep(5.0);
                atomic_store(&stats_on, 0);
                clock_gettime(CLOCK_MONOTONIC, &s1);
                double seconds = (s1.tv_sec - s0.tv_sec) + (s1.tv_nsec - s0.tv_nsec) * 1e-9;
                fprintf(stderr, "PORT_PROF trigger: stats only, %.1f fps\n", (double)(d3d9t_frames() - f0) / seconds);
                stats_report(seconds);
                continue;
            }
        }
        memmove(history, history + 1, 3 * sizeof history[0]);
        history[3] = d3d9t_frames();
        if (up < 45 || !history[0]) continue;
        double fps = (double)(history[3] - history[0]) / 3.0;
        low_run = fps < 25 ? low_run + 1 : 0;
        high_run = fps >= 55 ? high_run + 1 : 0;
        if (low_run >= 5 && low_reports < 3 && up - last_report > 60) {
            char label[64]; snprintf(label, sizeof label, "low frame rate (%.1f fps)", fps);
            prof_window(15, label); low_reports++; low_run = 0; last_report = up + 15;
        } else if (high_run >= 20 && high_reports < 1 && up > 120 && up - last_report > 30) {
            char label[64]; snprintf(label, sizeof label, "good frame rate (%.1f fps), for comparison", fps);
            prof_window(15, label); high_reports++; high_run = 0; last_report = up + 15;
        }
        if (low_reports >= 3 && high_reports >= 1 && !prof_trigger_path[0]) return;
    }
}

static void *prof_thread(void *arg) {
    (void)arg;
    /* This thread only sleeps: keep SIGPROF on the threads that burn CPU. */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &block, NULL);
    pthread_setname_np(pthread_self(), "port-prof");
    if (prof_auto_mode) { prof_auto(); return NULL; }
    prof_sleep(prof_start_after);
    prof_window(prof_duration, NULL);
    return NULL;
}

/* Called once at start-up (runtime/static_entry.c). */
void port_prof_init(void) {
    const char *spec = getenv("PORT_PROF");
    if (!spec || !*spec) return;
    if (getenv("PORT_PROF_TRIGGER")) snprintf(prof_trigger_path, sizeof prof_trigger_path, "%s", getenv("PORT_PROF_TRIGGER"));
    if (!strcmp(spec, "auto")) prof_auto_mode = 1;
    else if (sscanf(spec, "%lf:%lf:%ld", &prof_start_after, &prof_duration, &prof_period_us) < 2 || prof_duration <= 0) {
        fprintf(stderr, "PORT_PROF expects <start seconds>:<duration seconds>[:<period us>]\n");
        return;
    }
    if (prof_period_us < 100) prof_period_us = 100;
    prof_samples = calloc(PROF_MAX_SAMPLES, sizeof *prof_samples);
    if (!prof_samples) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    pthread_t thread;
    if (!pthread_create(&thread, NULL, prof_thread, NULL)) pthread_detach(thread);
}
