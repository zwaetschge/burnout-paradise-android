/* A14 -- machine lock, per-thread guest CPU state and the guest thread
 * launcher for the static lift. See runtime/static_threads.h for the contract.
 *
 * Nothing is faked: a guest thread is a real pthread, its stack and TEB are
 * real guest memory, its CPU state is the RECOMP_TLS register file of that
 * pthread, and a start address that was not translated ends in the same named
 * stop as the main path (never in a silent zero return).
 */
#define _GNU_SOURCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                                 /* syscall(), SYS_tgkill */
#endif
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <setjmp.h>
#include <signal.h>
#include <sched.h>
#include <dirent.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "static_threads.h"
void port_run_guest(uint32_t va, uint32_t sentinel);
#include "guest_thread_api.h"
#include "guest_sync_objects.h"
const char *port_act_enter(const char *what);   /* runtime/port_prof.c */
void port_act_leave(const char *previous);

/* ================================================================== */
/* the machine: one guest CPU, scheduled like a uniprocessor            */
/* ================================================================== */
/* Exactly one guest thread runs lifted code at a time ("holds the machine").
 * This used to be a plain recursive mutex. A mutex has no order: a thread
 * that let go of it for a yield usually took it straight back before any
 * waiter had woken up, and a thread that came back from a wait queued behind
 * whoever happened to win the race. Measured on the host in the game: 12,500
 * hand-overs per second, a quarter of them waiting 2.4 ms on average.
 *
 * It is now an explicit baton with a run queue, with the three rules a
 * uniprocessor Windows scheduler gives a program (all threads are treated as
 * one priority level; SetThreadPriority is recorded but not applied here):
 *   - a thread that becomes ready after a wait goes to the FRONT and the
 *     running thread gives way at its next check (the wait-completion boost);
 *   - a thread that yields (Sleep(0), SwitchToThread) or used up its time
 *     slice goes to the BACK, and the baton is handed to the next thread
 *     directly, so a yield always lets every other ready thread run first;
 *   - a thread that was pre-empted keeps its place ahead of the yielders.
 * With nobody waiting, taking and giving the baton is one small mutex. */
typedef struct StWaiter {
    struct StWaiter *next;
    pthread_cond_t cond;
    int granted;
    int front;          /* woke from a wait: runs before the yielders */
    int ready;          /* cond is initialised */
} StWaiter;

static pthread_mutex_t st_sched = PTHREAD_MUTEX_INITIALIZER;
static StWaiter *st_run_head, *st_run_tail;
static int st_busy;                         /* somebody holds the machine */
static _Atomic int st_waiters;              /* length of the run queue */
static _Atomic int st_preempt;              /* a woken thread is waiting at the front */
static struct timespec st_slice_start;      /* when the current holder got the machine */
static int st_owner_woken;                  /* the holder itself came from a wait: not pre-empted in its slice */
#define ST_SLICE_NS 4000000ll               /* time slice among ready threads: 4 ms */

static RECOMP_TLS int st_machine_depth;
static RECOMP_TLS StWaiter st_waiter;

/* Statistics for the profiler (runtime/port_prof.c). */
static _Atomic uint64_t st_machine_acquired, st_machine_contended, st_machine_wait_ns;
void st_machine_stats(uint64_t *acquired, uint64_t *contended, uint64_t *wait_ns) {
    *acquired = st_machine_acquired; *contended = st_machine_contended; *wait_ns = st_machine_wait_ns;
}
typedef struct { _Atomic uint64_t wait_ns, taken, contended; int tid; } StLockUser;
#define ST_LOCK_USERS 96
static StLockUser st_lock_users[ST_LOCK_USERS];
static _Atomic int st_lock_user_count;
static RECOMP_TLS StLockUser *st_lock_me;
static StLockUser *st_lock_user(void) {
    if (st_lock_me) return st_lock_me;
    int index = atomic_fetch_add(&st_lock_user_count, 1);
    static StLockUser overflow;
    st_lock_me = index < ST_LOCK_USERS ? &st_lock_users[index] : &overflow;
    st_lock_me->tid = (int)syscall(SYS_gettid);
    return st_lock_me;
}
void st_machine_report_reset(void) {
    for (int i = 0; i < ST_LOCK_USERS; i++) { st_lock_users[i].wait_ns = 0; st_lock_users[i].taken = 0; st_lock_users[i].contended = 0; }
}
void st_machine_report(void) {
    int count = atomic_load(&st_lock_user_count);
    for (int i = 0; i < count && i < ST_LOCK_USERS; i++) {
        StLockUser *u = &st_lock_users[i];
        if (!u->taken) continue;
        char comm[40] = "?", path[64];
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", u->tid);
        FILE *f = fopen(path, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0; fclose(f); }
        fprintf(stderr, "PORT_PROF lock %-16s tid %-7d taken=%-8llu contended=%-7llu waited=%.2fs\n", comm, u->tid,
                (unsigned long long)u->taken, (unsigned long long)u->contended, (double)u->wait_ns / 1e9);
    }
}

/* Take the machine. front: 1 = this thread just became ready after a wait,
 * 0 = it yielded / starts / was pre-empted (`ahead` keeps a pre-empted thread
 * in front of the yielders). */
static void st_machine_take(int front, int ahead) {
    StLockUser *me = st_lock_user();
    st_machine_acquired++;
    me->taken++;
    pthread_mutex_lock(&st_sched);
    if (!st_busy) {
        st_busy = 1;
        st_owner_woken = front;
        clock_gettime(CLOCK_MONOTONIC, &st_slice_start);
        pthread_mutex_unlock(&st_sched);
        return;
    }
    StWaiter *w = &st_waiter;
    if (!w->ready) { pthread_cond_init(&w->cond, NULL); w->ready = 1; }
    w->granted = 0;
    w->front = front;
    w->next = NULL;
    if (front || ahead) {
        /* behind the woken threads that are already queued, ahead of the rest */
        StWaiter **link = &st_run_head;
        while (*link && (*link)->front) link = &(*link)->next;
        if (ahead && !front) { /* a pre-empted thread: same place, it is not "front" itself */ }
        w->next = *link;
        *link = w;
        if (!w->next) st_run_tail = w;
    } else {
        if (st_run_tail) st_run_tail->next = w; else st_run_head = w;
        st_run_tail = w;
    }
    atomic_fetch_add(&st_waiters, 1);
    if (front && !st_owner_woken) atomic_store(&st_preempt, 1);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const char *activity = port_act_enter("(machine wait)");
    while (!w->granted) pthread_cond_wait(&w->cond, &st_sched);
    pthread_mutex_unlock(&st_sched);
    port_act_leave(activity);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t waited = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    st_machine_contended++;
    st_machine_wait_ns += waited;
    me->contended++;
    me->wait_ns += waited;
}

/* Give the machine to the thread at the head of the run queue (the baton is
 * passed, never dropped while somebody waits). */
static void st_machine_give(void) {
    pthread_mutex_lock(&st_sched);
    StWaiter *w = st_run_head;
    if (w) {
        st_run_head = w->next;
        if (!st_run_head) st_run_tail = NULL;
        atomic_fetch_sub(&st_waiters, 1);
        w->granted = 1;
        st_owner_woken = w->front;
        clock_gettime(CLOCK_MONOTONIC, &st_slice_start);
        atomic_store(&st_preempt, st_run_head && st_run_head->front && !st_owner_woken ? 1 : 0);
        pthread_cond_signal(&w->cond);
    } else {
        st_busy = 0;
        atomic_store(&st_preempt, 0);
    }
    pthread_mutex_unlock(&st_sched);
}

void st_machine_lock(void) {
    if (st_machine_depth++ == 0) st_machine_take(0, 0);
}

void st_machine_unlock(void) {
    if (st_machine_depth <= 0) return;
    if (--st_machine_depth == 0) st_machine_give();
}

int st_machine_held(void) { return st_machine_depth > 0; }

/* Around a host call that blocks: let go, and come back as a woken thread. */
int st_machine_release(void) {
    int depth = st_machine_depth;
    if (depth <= 0) return 0;
    st_machine_depth = 0;
    st_machine_give();
    return depth;
}

void st_machine_reacquire(int held) {
    if (!held) return;
    st_machine_take(1, 0);
    st_machine_depth = held;
}

/* Sleep(0) / SwitchToThread: every other ready thread runs before this one
 * continues. Returns 1 when the machine was handed over, 0 when nobody else
 * was ready (the documented SwitchToThread result). */
int st_machine_switch(void) {
    if (st_machine_depth <= 0 || !atomic_load_explicit(&st_waiters, memory_order_relaxed)) return 0;
    int depth = st_machine_depth;
    st_machine_depth = 0;
    st_machine_give();
    st_machine_take(0, 0);
    st_machine_depth = depth;
    return 1;
}

/* The lifted code calls this from RECOMP_BACKEDGE (every RECOMP_YIELD_EVERY
 * backward branches) and the import thunk after every API call: the running
 * thread gives way to a thread that woke up, or, among equals, when its time
 * slice is over. With an empty run queue it is two loads. */
void st_machine_yield(void) {
    if (st_machine_depth <= 0 || !atomic_load_explicit(&st_waiters, memory_order_relaxed)) return;
    int preempted = atomic_load_explicit(&st_preempt, memory_order_relaxed);
    if (!preempted) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long ran = (now.tv_sec - st_slice_start.tv_sec) * 1000000000ll + (now.tv_nsec - st_slice_start.tv_nsec);
        if (ran < ST_SLICE_NS) return;
    }
    int depth = st_machine_depth;
    st_machine_depth = 0;
    st_machine_give();
    st_machine_take(0, preempted);      /* pre-empted: ahead of the yielders; slice over: to the back */
    st_machine_depth = depth;
}
int st_machine_preempt_pending(void) { return atomic_load_explicit(&st_preempt, memory_order_relaxed); }

/* Where the guest threads run.
 *
 * Only one guest thread executes at a time, so together they need about one
 * core. Left to the scheduler they are spread over all cores, each of which is
 * then busy a fraction of the time: the frequency governor clocks those cores
 * down (measured on the Odin 3: 3.3 of 4.3 GHz on the prime cores, 2.2 of 3.5
 * on the others) and every hand-over crosses to another core. Kept on the
 * fastest cores they stay at top frequency: 40-48 fps became 56-60 while
 * driving.
 *
 * Not on ONE core, though (the first version): a guest thread that the game
 * wakes (a job worker after SetEvent or its 1 ms poll) first has to be put on
 * a CPU by the kernel before it can take the machine; with every guest thread
 * on one core it queued behind the thread that holds the machine and spins in
 * the game's job-wait loop (SleepEx(0), millions per second). How long the
 * kernel let it wait depended on its refresh-rate-tuned scheduling: at 60 Hz
 * the game ran, with the panel at 120 Hz it fell into a slow state (worker
 * wake-up 0.9 -> 2.4 ms, 8-25 fps, CPU and GPU mostly idle). With the two prime
 * cores a woken thread gets on the second one at once: 59-60 fps at 120 Hz.
 *
 *   PORT_GUEST_CPUS=<hex mask>   explicit CPU set
 *   PORT_GUEST_CPUS=0            leave it to the scheduler
 *   unset                        Android: all cores of the highest
 *                                cpuinfo_max_freq (Odin 3: cpu6+cpu7);
 *                                host: leave it to the scheduler */
static long st_fastest_cpu_mask(void) {
    long freq[32] = { 0 }, best_freq = 0;
    for (int cpu = 0; cpu < 32; cpu++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fscanf(f, "%ld", &freq[cpu]) == 1 && freq[cpu] > best_freq) best_freq = freq[cpu];
        fclose(f);
    }
    long mask = 0;
    for (int cpu = 0; cpu < 32; cpu++) if (best_freq && freq[cpu] == best_freq) mask |= 1L << cpu;
    if (mask) fprintf(stderr, "PORT_GUEST_CPUS auto: mask 0x%lx (max %ld kHz)\n", mask, best_freq);
    return mask;
}
static long st_guest_mask = -1;
void st_apply_guest_affinity(void) {
    long mask = st_guest_mask;
    if (mask < 0) {
        const char *text = getenv("PORT_GUEST_CPUS");
        if (text && *text) {
            mask = strtol(text, NULL, 16);
            fprintf(stderr, "PORT_GUEST_CPUS mask=0x%lx\n", mask);
        } else {
#ifdef __ANDROID__
            mask = st_fastest_cpu_mask();
#else
            mask = 0;
#endif
        }
        st_guest_mask = mask;
    }
    if (!mask) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int cpu = 0; cpu < 32; cpu++) if (mask & (1L << cpu)) CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set)) perror("PORT_GUEST_CPUS sched_setaffinity");
}

/* Threads the host libraries create from a guest thread (DXVK's command
 * stream, queue and submit threads, shader compilers, audio) inherit the
 * guest core. They then took a fifth of the one core the guest runs on
 * (Odin 3, 120 Hz: dxvk-cs ~20 % on cpu7) while the second prime core stayed
 * idle. Every thread of the process that is not a guest thread ("g-…", the
 * main guest thread "burnout-game") is moved to the other cores. Called
 * periodically (new host threads appear after device creation, Reset, audio
 * start); cheap, it only reads /proc/self/task. */
void st_isolate_host_threads(void) {
    if (st_guest_mask <= 0) return;
    cpu_set_t host;
    CPU_ZERO(&host);
    long online = sysconf(_SC_NPROCESSORS_CONF);
    int any = 0;
    for (int cpu = 0; cpu < online && cpu < 32; cpu++)
        if (!(st_guest_mask & (1L << cpu))) { CPU_SET(cpu, &host); any = 1; }
    if (!any) return;
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return;
    static int reported;
    int moved = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        pid_t tid = (pid_t)atoi(entry->d_name);
        char path[64], comm[32] = "";
        snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        if (!strncmp(comm, "g-", 2) || !strcmp(comm, "burnout-game")) continue;
        cpu_set_t now;
        CPU_ZERO(&now);
        if (sched_getaffinity(tid, sizeof now, &now) == 0 && CPU_EQUAL(&now, &host)) continue;
        if (sched_setaffinity(tid, sizeof host, &host) == 0) {
            moved++;
            if (reported < 40) { fprintf(stderr, "PORT_GUEST_CPUS host thread %s (%d) -> other cores\n", comm, (int)tid); reported++; }
        }
    }
    closedir(dir);
    (void)moved;
}

/* ================================================================== */
/* process context                                                    */
/* ================================================================== */

typedef struct {
    uint32_t thread_id;
    uint32_t handle;
    int bound;
    int has_pending_exit;
    uint32_t pending_exit;
    uint32_t teb;
    pthread_t host;
    int started;
} StThread;

static GuestMemory *st_memory;
static uint32_t st_peb;
static uint32_t st_process_id = 0x1000u;
static uint32_t st_tls_template;
static uint32_t st_tls_slots = ST_TLS_VECTOR_SLOTS;
static void (*st_stop_hook)(const char *kind, uint32_t target);

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
/* One completion condition per slot. pthread_timedjoin_np() is a glibc
 * extension that bionic (android29) does not declare, so a guest thread
 * announces its exit through its own condvar and st_thread_join_all() waits on
 * that before the ordinary pthread_join(). Portable, and it also makes the
 * "thread finished before its handle was bound" case explicit. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
} StDone;
static StDone st_done[ST_GUEST_MAX_THREADS];
static pthread_once_t st_done_once = PTHREAD_ONCE_INIT;
static void st_done_init(void) {
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) {
        pthread_mutex_init(&st_done[i].mutex, NULL);
        pthread_cond_init(&st_done[i].cond, NULL);
        st_done[i].done = 0;
    }
}
static StThread st_threads[ST_GUEST_MAX_THREADS];
static uint32_t st_thread_total;
static uint32_t st_slot_next;
extern RECOMP_TLS uint32_t g_teb;   /* runtime/static_entry.c */
static RECOMP_TLS uint32_t st_self_teb;
static RECOMP_TLS uint32_t st_self_id;

void st_thread_set_context(GuestMemory *memory, uint32_t peb, uint32_t process_id,
                           uint32_t tls_template, uint32_t tls_slots) {
    pthread_mutex_lock(&st_lock);
    st_memory = memory;
    st_peb = peb;
    if (process_id) st_process_id = process_id;
    st_tls_template = tls_template;
    if (tls_slots && tls_slots <= 1024) st_tls_slots = tls_slots;
    pthread_mutex_unlock(&st_lock);
}

void st_thread_set_stop_hook(void (*fn)(const char *kind, uint32_t target)) {
    pthread_mutex_lock(&st_lock);
    st_stop_hook = fn;
    pthread_mutex_unlock(&st_lock);
}

uint32_t st_thread_teb(void) { return st_self_teb; }
uint32_t st_thread_id(void) { return st_self_id; }

/* ================================================================== */
/* guest TEB                                                          */
/* ================================================================== */

int st_teb_init(GuestMemory *memory, uint32_t teb, uint32_t self,
                uint32_t stack_base, uint32_t stack_limit, uint32_t process_id,
                uint32_t thread_id, uint32_t peb, uint32_t tls_vector) {
    if (!memory) return GM_UNSUPPORTED;
    uint8_t block[ST_TEB_SIZE];
    memset(block, 0, sizeof block);
    uint32_t *w = (uint32_t *)block;
    w[0] = 0xFFFFFFFFu;      /* +0x00 SEH chain end */
    w[1] = stack_base;       /* +0x04 StackBase */
    w[2] = stack_limit;      /* +0x08 StackLimit */
    w[5] = self;             /* +0x14 Self */
    w[6] = 0;                /* +0x18 EnvironmentPointer */
    w[7] = process_id;       /* +0x1c ClientId.ProcessId (NT layout) */
    w[8] = process_id;       /* +0x20 ProcessId (project layout, guest_peb.h) */
    w[9] = thread_id;        /* +0x24 ThreadId */
    w[10] = 0;               /* +0x28 ActiveRpcHandle */
    w[11] = tls_vector;      /* +0x2c ThreadLocalStoragePointer */
    w[12] = peb;             /* +0x30 PEB */
    w[13] = 0;               /* +0x34 LastError */
    return gm_write(memory, teb, block, sizeof block);
}

/* ================================================================== */
/* thread bookkeeping                                                 */
/* ================================================================== */

static StThread *slot_for(uint32_t thread_id) {
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++)
        if (st_threads[i].started && st_threads[i].thread_id == thread_id)
            return &st_threads[i];
    return NULL;
}

void st_thread_bind_handle(uint32_t thread_id, uint32_t handle) {
    uint32_t pending = 0;
    int has_pending = 0;
    pthread_mutex_lock(&st_lock);
    StThread *t = slot_for(thread_id);
    if (t) {
        t->handle = handle;
        t->bound = 1;
        has_pending = t->has_pending_exit;
        pending = t->pending_exit;
    }
    pthread_mutex_unlock(&st_lock);
    /* The guest thread may already have finished before the bridge returned its
     * handle; signal it now instead of losing the exit. */
    if (has_pending) gta_note_thread_exit(handle, pending);
}

uint32_t st_thread_count(void) {
    uint32_t n = 0;
    pthread_mutex_lock(&st_lock);
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) n += st_threads[i].started ? 1u : 0u;
    pthread_mutex_unlock(&st_lock);
    return n;
}

void st_thread_join_all(uint32_t milliseconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += (time_t)(milliseconds / 1000u);
    deadline.tv_nsec += (long)((milliseconds % 1000u) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_once(&st_done_once, st_done_init);
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) {
        pthread_t host;
        int started;
        pthread_mutex_lock(&st_lock);
        started = st_threads[i].started;
        host = st_threads[i].host;
        pthread_mutex_unlock(&st_lock);
        if (!started) continue;
        /* Wait (bounded) for the slot's completion flag, then reap it. */
        pthread_mutex_lock(&st_done[i].mutex);
        while (!st_done[i].done) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                pthread_mutex_unlock(&st_done[i].mutex);
                return;
            }
            struct timespec wait = {0, 0};
            wait.tv_sec = deadline.tv_sec - now.tv_sec;
            long ns = deadline.tv_nsec - now.tv_nsec;
            if (ns < 0) { wait.tv_sec--; ns += 1000000000L; }
            wait.tv_nsec = ns;
            pthread_cond_timedwait(&st_done[i].cond, &st_done[i].mutex, &wait);
        }
        pthread_mutex_unlock(&st_done[i].mutex);
        pthread_join(host, NULL);
    }
}

/* ================================================================== */
/* launcher                                                           */
/* ================================================================== */

typedef struct {
    uint32_t thread_id;
    uint32_t start;
    uint32_t parameter;
    uint32_t stack_size;
    uint32_t teb;
    uint32_t stack_base;
    uint32_t stack_top;
} StLaunch;

static void st_report_exit(uint32_t thread_id, uint32_t code) {
    uint32_t handle = 0;
    int bound = 0;
    pthread_mutex_lock(&st_lock);
    StThread *t = slot_for(thread_id);
    if (t) {
        if (t->bound) {
            handle = t->handle;
            bound = 1;
        } else {
            t->has_pending_exit = 1;
            t->pending_exit = code;
        }
    }
    pthread_mutex_unlock(&st_lock);
    if (bound) gta_note_thread_exit(handle, code);
    gta_note_thread_exit_id(thread_id, code);   /* handles from DuplicateHandle(GetCurrentThread()) */
}

static void st_note_kernel_tid(uint32_t thread_id, pid_t tid);
static RECOMP_TLS jmp_buf st_exit_jump;
static RECOMP_TLS int st_exit_armed;
/* Ends the calling guest thread (ExitThread). Returns only when the caller is
 * not a launched guest thread (the main thread). */
void st_thread_exit_current(void) {
    if (st_exit_armed) longjmp(st_exit_jump, 1);
}

static void *st_thread_main(void *arg) {
    StLaunch launch = *(StLaunch *)arg;
    free(arg);

    /* A new guest thread starts from the documented x86 reset state; the
     * RECOMP_TLS variables of this pthread were initialised that way already,
     * these assignments make it explicit and independent of the initialiser. */
    g_esp = launch.stack_top;
    g_eax = g_ebx = g_ecx = g_edx = g_esi = g_edi = g_ebp = 0;
    g_cur_func = launch.start;
    g_fs_base = launch.teb;
    g_gs_base = 0;
    g_es_base = 0;
    g_fp_top = 0;
    g_fpu_cw = 0x037Fu;
    g_mxcsr = 0x1F80u;
    g_flag_k = FK_NONE;
    g_flag_a = g_flag_b = g_flag_cf = 0;
    g_alias_entry = 0;
    g_icall_count = 0;
    g_icall_trace_idx = 0;
    st_self_teb = launch.teb;
    st_self_id = launch.thread_id;
    /* The bridges address the thread's TEB through g_teb (LastError, the SEH
     * chain walked by RaiseException, TLS): it must be this thread's, not the
     * main thread's default. */
    g_teb = launch.teb;

    uint32_t exit_code = 0;
    volatile int exited = 0;
    st_note_kernel_tid(launch.thread_id, (pid_t)syscall(SYS_gettid));
    st_apply_guest_affinity();
    {   /* Name the host thread after the guest start routine (profiles, top -H). */
        char name[16];
        snprintf(name, sizeof name, "g-%08X", launch.start);
        pthread_setname_np(pthread_self(), name);
    }
    st_machine_lock();
    /* Win32 stdcall thread routine: one argument, callee pops it. */
    PUSH32(g_esp, launch.parameter);
    PUSH32(g_esp, RECOMP_RETADDR);   /* return address on top, as CreateThread's caller frame */
    recomp_func_t fn = recomp_lookup(launch.start);
    if (!fn) {
        void (*hook)(const char *, uint32_t) = st_stop_hook;
        st_machine_unlock();
        if (hook) hook("thread-start-untranslated", launch.start);
        exit_code = 0;
    } else {
        (void)fn;
        /* ExitThread (CRT _endthreadex) never returns to the guest: the bridge
         * has already published the exit code, st_thread_exit_current() unwinds
         * the host frames of this thread back to here. */
        st_exit_armed = 1;
        if (!setjmp(st_exit_jump)) {
            port_run_guest(launch.start, RECOMP_RETADDR);
            exit_code = g_eax;
        } else {
            exited = 1;
        }
        st_exit_armed = 0;
        st_machine_unlock();
    }
    st_note_kernel_tid(launch.thread_id, 0);
    if (!exited) st_report_exit(launch.thread_id, exit_code);
    /* Announce completion to st_thread_join_all(). */
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) {
        if (st_threads[i].thread_id != launch.thread_id) continue;
        pthread_mutex_lock(&st_done[i].mutex);
        st_done[i].done = 1;
        pthread_cond_broadcast(&st_done[i].cond);
        pthread_mutex_unlock(&st_done[i].mutex);
        break;
    }
    return NULL;
}

int st_thread_launch(uint32_t thread_id, uint32_t start, uint32_t parameter,
                     uint32_t stack_size) {
    GuestMemory *memory;
    uint32_t peb, process_id, template_vector, slots;
    pthread_mutex_lock(&st_lock);
    memory = st_memory;
    peb = st_peb;
    process_id = st_process_id;
    template_vector = st_tls_template;
    slots = st_tls_slots;
    if (!memory || st_slot_next >= ST_GUEST_MAX_THREADS) {
        pthread_mutex_unlock(&st_lock);
        return GM_UNSUPPORTED;
    }
    if (!stack_size || stack_size > ST_GUEST_STACK_SIZE) stack_size = ST_GUEST_STACK_SIZE;
    stack_size = (stack_size + 0xFFFu) & ~0xFFFu;
    uint32_t slot = st_slot_next++;
    pthread_mutex_unlock(&st_lock);
    pthread_once(&st_done_once, st_done_init);

    uint32_t base = ST_GUEST_STACK_ARENA + slot * ST_GUEST_SLOT_STRIDE;
    if (base + ST_GUEST_SLOT_STRIDE > ST_GUEST_STACK_LIMIT) return GM_UNSUPPORTED;
    uint32_t teb = base + ST_GUEST_STACK_SIZE;
    uint32_t vector = teb + ST_TEB_SIZE;
    uint32_t stack_base = base;
    uint32_t stack_top = base + stack_size;

    /* Zero the slot so a reused slot cannot leak a previous thread's stack. */
    uint8_t zero[4096];
    memset(zero, 0, sizeof zero);
    for (uint32_t off = 0; off < ST_GUEST_SLOT_STRIDE; off += sizeof zero)
        if (gm_write(memory, base + off, zero, sizeof zero)) return GM_UNSUPPORTED;

    int rc = st_teb_init(memory, teb, teb, stack_base, stack_base, process_id,
                         thread_id, peb, vector);
    if (rc) return rc;
    /* Static TLS: per-thread vector, initialised from the main thread's copy. */
    if (template_vector && slots) {
        uint8_t cell[4];
        for (uint32_t i = 0; i < slots; i++) {
            if (gm_read(memory, template_vector + i * 4, cell, 4)) return GM_UNSUPPORTED;
            if (gm_write(memory, vector + i * 4, cell, 4)) return GM_UNSUPPORTED;
        }
    }

    StLaunch *launch = calloc(1, sizeof *launch);
    if (!launch) return GM_UNSUPPORTED;
    launch->thread_id = thread_id;
    launch->start = start;
    launch->parameter = parameter;
    launch->stack_size = stack_size;
    launch->teb = teb;
    launch->stack_base = stack_base;
    launch->stack_top = stack_top;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t host;
    int err = pthread_create(&host, &attr, st_thread_main, launch);
    pthread_attr_destroy(&attr);
    if (err) {
        free(launch);
        return GM_UNSUPPORTED;
    }

    pthread_mutex_lock(&st_lock);
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) {
        if (st_threads[i].started) continue;
        memset(&st_threads[i], 0, sizeof st_threads[i]);
        st_threads[i].started = 1;
        st_threads[i].thread_id = thread_id;
        st_threads[i].teb = teb;
        st_threads[i].host = host;
        st_thread_total++;
        break;
    }
    pthread_mutex_unlock(&st_lock);
    fprintf(stderr,
            "PORT_THREAD start tid=0x%08X va=0x%08X arg=0x%08X stack=0x%08X..0x%08X "
            "teb=0x%08X\n",
            thread_id, start, parameter, stack_base, stack_top, teb);
    fflush(stderr);
    return 0;
}

/* Diagnostics: deliver `sig` to every *running* guest thread except the
 * caller. Threads are addressed by kernel thread id: a pthread_t of a detached
 * thread that has exited is dangling (pthread_kill on it deadlocked the
 * signal handler of the caller). */
static struct { uint32_t thread_id; pid_t tid; } st_tids[ST_GUEST_MAX_THREADS];
static pthread_mutex_t st_tid_lock = PTHREAD_MUTEX_INITIALIZER;
/* tid != 0: the calling thread starts; tid == 0: it ends. */
static void st_note_kernel_tid(uint32_t thread_id, pid_t tid) {
    pthread_mutex_lock(&st_tid_lock);
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) {
        if (tid ? st_tids[i].tid != 0 : st_tids[i].thread_id != thread_id || !st_tids[i].tid) continue;
        st_tids[i].thread_id = thread_id; st_tids[i].tid = tid;
        break;
    }
    pthread_mutex_unlock(&st_tid_lock);
}
void st_signal_others(int sig) {
    pid_t self = (pid_t)syscall(SYS_gettid), process = getpid();
    for (uint32_t i = 0; i < ST_GUEST_MAX_THREADS; i++) {       /* no lock: called from a signal handler */
        pid_t tid = st_tids[i].tid;
        if (tid > 0 && tid != self) syscall(SYS_tgkill, process, tid, sig);
    }
    if (self != process) syscall(SYS_tgkill, process, process, sig);   /* the main thread */
}
