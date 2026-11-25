/*
 * Burnout Paradise Remastered -> Android arm64-v8a
 * A17: Android host front end (libport_main.so).
 *
 * No game code, no assets and no license data live in this file. It is only the
 * Android side of the contract described in android/jni/port_host_api.h:
 *
 *   - the Android looper thread stays the UI/lifecycle thread;
 *   - the statically recompiled image (libburnout_static.so) runs
 *     port_entry(game_root, user_root) on its own pthread with a 16 MiB host
 *     stack;
 *   - ANativeWindow*, surface size, input and lifecycle events are forwarded
 *     through *optional* dlsym()'d symbols; a missing symbol is one logcat
 *     line, never a crash and never a silent stub;
 *   - the runtime's stdout/stderr go through a pipe into logcat, and every
 *     "PORT_TRAP" line is additionally appended to
 *     <files>/port-last-trap.txt (also when the image calls _exit(70)).
 *
 * Game data is read from the user's own directory
 *   /sdcard/Android/data/dev.plum.burnoutport/files/BurnoutPR
 * (ANativeActivity::externalDataPath + "/BurnoutPR"); the writable user profile
 * goes to internalDataPath + "/user".
 */

#include <android/configuration.h>
#include <android/api-level.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/window.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "android_native_app_glue.h"
#include "port_host_api.h"

#define LOG_TAG "burnoutport"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define STATIC_LIB_NAME "libburnout_static.so"
#define SELF_LIB_NAME "libport_main.so"
#define GAME_DATA_FALLBACK \
    "/sdcard/Android/data/dev.plum.burnoutport/files/BurnoutPR"
#define FILES_FALLBACK \
    "/sdcard/Android/data/dev.plum.burnoutport/files"

/* The guest flat address space that runtime/static_entry.c reserves. Probed
 * here once, before any game code runs, so a device that cannot hand out 4 GiB
 * of virtual address space says so in logcat instead of failing deep inside
 * the loader. */
#define GUEST_FLAT_SIZE ((size_t)4u << 30)

/* ------------------------------------------------------------------ */
/* optional symbol table                                              */
/* ------------------------------------------------------------------ */

typedef int (*fn_entry)(const char *game_root, const char *user_root);
typedef int (*fn_entry_ex)(const char *game_root, const char *user_root,
                           const char *nls_path);
typedef int (*fn_set_native_window)(ANativeWindow *window, int width, int height);
typedef int (*fn_set_surface_size)(int width, int height);
typedef int (*fn_set_gamepad_state)(const port_gamepad_state *state);
typedef int (*fn_post_host_input)(uint32_t type, uint32_t x, uint32_t y,
                                  uint32_t code);
typedef int (*fn_on_lifecycle)(uint32_t event, void *data);
typedef void (*fn_void)(void);
typedef int (*fn_gu_set_surface_size)(void *user32, uint32_t w, uint32_t h);
typedef int (*fn_gu_post_host_input)(void *user32, const port_gu_host_input *in);

typedef struct port_api {
    fn_entry entry;                       /* mandatory */
    fn_entry_ex entry_ex;                 /* optional */
    fn_set_native_window set_native_window;
    fn_set_surface_size set_surface_size;
    fn_set_gamepad_state set_gamepad_state;
    fn_post_host_input post_host_input;
    fn_on_lifecycle on_lifecycle;
    fn_void on_pause;
    fn_void on_resume;
    fn_void on_window_destroy;
    /* runtime/guest_user32.h fallback (needs the g_user32 data symbol) */
    fn_gu_set_surface_size gu_set_surface_size;
    fn_gu_post_host_input gu_post_host_input;
    void **gu_user32;
} port_api;

/* ------------------------------------------------------------------ */
/* host state                                                         */
/* ------------------------------------------------------------------ */

struct host {
    struct android_app *app;

    char files_dir[PATH_MAX];   /* externalDataPath (…/files) */
    char game_root[PATH_MAX];   /* files/BurnoutPR */
    char user_root[PATH_MAX];   /* internalDataPath/user */
    char trap_path[PATH_MAX];   /* files/port-last-trap.txt */
    char log_path[PATH_MAX];    /* files/port-runtime.log */

    ANativeWindow *window;
    int32_t width, height;
    int window_owned_by_image;
    int heartbeat_enabled;

    EGLDisplay display;
    EGLSurface surface;
    EGLContext context;
    int animating;
    int64_t frames;

    pthread_t thread;
    int thread_started;
    volatile int thread_running;
    volatile int thread_done;
    int thread_rc;

    void *image;
    port_api api;

    port_gamepad_state pad;
    int pad_connected;
    int input_sink_missing_logged;
    int pointer_extra_logged;
};

static struct host g_host;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *path) {
    if (!path || !*path) return -1;
    char tmp[PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (mkdir(tmp, 0770) != 0 && errno != EEXIST) return -1;
        tmp[i] = '/';
    }
    if (mkdir(tmp, 0770) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int join_path(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Derive the directory of this shared library from /proc/self/maps so a
 * sibling library can be dlopen()'d without any JNI plumbing. */
static int discover_lib_dir(char *out, size_t out_size) {
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) {
        LOGW("cannot open /proc/self/maps: %s", strerror(errno));
        return 0;
    }
    char line[PATH_MAX + 128];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *hit = strstr(line, SELF_LIB_NAME);
        if (!hit) continue;
        char *slash = strrchr(line, '/');
        if (!slash || slash >= hit) continue;
        size_t len = (size_t)(slash - line);
        if (len == 0 || len + 1 >= out_size) continue;
        memcpy(out, line, len);
        out[len] = '\0';
        found = 1;
        break;
    }
    fclose(f);
    if (!found) LOGW("%s not found in /proc/self/maps", SELF_LIB_NAME);
    return found;
}

/* ------------------------------------------------------------------ */
/* stdout/stderr -> logcat, PORT_TRAP -> files/port-last-trap.txt     */
/* ------------------------------------------------------------------ */

static int g_log_rd = -1;
static int g_log_wr = -1;
static int g_log_mirror_fd = -1;
static off_t g_log_mirror_bytes;
static pthread_t g_log_thread;
static int g_log_thread_started;
static volatile int g_log_dying;
static volatile int g_log_drained;

/* logcat rotates; the mirror file keeps the tail of the run readable after the
 * fact. Capped so a long session cannot fill the user's data directory. */
#define LOG_MIRROR_MAX_BYTES (8 * 1024 * 1024)

static void log_mirror_write(const char *line) {
    if (g_log_mirror_fd < 0) return;
    size_t len = strlen(line);
    if (g_log_mirror_bytes + (off_t)len + 1 > LOG_MIRROR_MAX_BYTES) return;
    ssize_t n = write(g_log_mirror_fd, line, len);
    if (n > 0) g_log_mirror_bytes += n;
    n = write(g_log_mirror_fd, "\n", 1);
    if (n > 0) g_log_mirror_bytes += n;
}

static void trap_file_append(const char *line) {
    if (!g_host.trap_path[0]) return;
    int fd = open(g_host.trap_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0660);
    if (fd < 0) return;
    struct timespec ts;
    char stamp[64];
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    int n = snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                     tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
    if (n > 0) {
        ssize_t ignored = write(fd, stamp, (size_t)n);
        (void)ignored;
    }
    size_t len = strlen(line);
    ssize_t ignored = write(fd, line, len);
    (void)ignored;
    if (len == 0 || line[len - 1] != '\n') {
        ignored = write(fd, "\n", 1);
        (void)ignored;
    }
    close(fd);
}

static void log_emit_line(const char *line) {
    if (!*line) return;
    log_mirror_write(line);
    if (strncmp(line, "PORT_TRAP", 9) == 0) {
        LOGE("runtime: %s", line);
        trap_file_append(line);
        return;
    }
    LOGI("runtime: %s", line);
}

static void *log_reader_thread(void *arg) {
    (void)arg;
    char buf[4096];
    char line[2048];
    size_t linelen = 0;
    struct pollfd pfd;
    pfd.fd = g_log_rd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            if (g_log_dying) {
                g_log_drained = 1;
                return NULL;
            }
            continue;
        }
        ssize_t n = read(g_log_rd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (g_log_dying) {
                    g_log_drained = 1;
                    return NULL;
                }
                continue;
            }
            break;
        }
        if (n == 0) {
            if (g_log_dying) {
                g_log_drained = 1;
                return NULL;
            }
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') {
                line[linelen] = '\0';
                log_emit_line(line);
                linelen = 0;
                continue;
            }
            if (c == '\r') continue;
            if (linelen + 1 < sizeof(line)) line[linelen++] = c;
            /* over-long lines are truncated, never split silently */
        }
    }
    g_log_drained = 1;
    return NULL;
}

static void redirect_streams_to_logcat(void) {
    int fds[2];
    if (pipe(fds) != 0) {
        LOGW("pipe() failed: %s; runtime output stays on the default fds",
             strerror(errno));
        return;
    }
    g_log_rd = fds[0];
    g_log_wr = fds[1];
    fcntl(g_log_rd, F_SETFD, FD_CLOEXEC);
    fcntl(g_log_wr, F_SETFD, FD_CLOEXEC);
    fcntl(g_log_rd, F_SETFL, fcntl(g_log_rd, F_GETFL, 0) | O_NONBLOCK);

    fflush(stdout);
    fflush(stderr);
    if (dup2(g_log_wr, STDOUT_FILENO) < 0 || dup2(g_log_wr, STDERR_FILENO) < 0) {
        LOGW("dup2() failed: %s", strerror(errno));
        return;
    }
    /* Line buffered: the runtime writes one line per event and expects it to
     * appear immediately (that is what a trap report depends on). */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (pthread_create(&g_log_thread, NULL, log_reader_thread, NULL) != 0) {
        LOGW("log reader thread could not be created");
        return;
    }
    g_log_thread_started = 1;
    LOGI("runtime stdout/stderr -> logcat (tag %s, prefix \"runtime:\")", LOG_TAG);
    if (g_host.log_path[0]) {
        g_log_mirror_fd = open(g_host.log_path,
                               O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
        if (g_log_mirror_fd >= 0) {
            LOGI("runtime log mirror: %s (capped at %d MiB)", g_host.log_path,
                 LOG_MIRROR_MAX_BYTES >> 20);
        } else {
            LOGW("cannot open log mirror %s: %s", g_host.log_path,
                 strerror(errno));
        }
    }
}

/* The image terminates a trap with _exit(70) (runtime/static_entry.c). The pipe
 * reader is the only consumer of the trap line, so before the process really
 * dies we give it a bounded moment to drain and write the trap file. */
static void drain_log_pipe(int max_ms) {
    if (!g_log_thread_started) return;
    g_log_dying = 1;
    for (int waited = 0; waited < max_ms; waited += 5) {
        if (g_log_drained) break;
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (g_log_drained) return;
    /* The reader thread did not finish in time (or never ran): take over and
     * drain the pipe here, so the trap line still reaches the trap file. */
    char buf[1024];
    char line[2048];
    size_t linelen = 0;
    ssize_t n;
    while ((n = read(g_log_rd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[linelen] = '\0';
                log_emit_line(line);
                linelen = 0;
            } else if (buf[i] != '\r' && linelen + 1 < sizeof(line)) {
                line[linelen++] = buf[i];
            }
        }
    }
    if (linelen) {
        line[linelen] = '\0';
        log_emit_line(line);
    }
    LOGW("log pipe drained by the exiting thread (%d ms timeout)", max_ms);
}

typedef void (*fn_exit)(int);
static fn_exit g_real_exit;
static volatile int g_in_exit;

/* Interposed so that `_exit(70)` inside the static image still lets the trap
 * line reach files/port-last-trap.txt. libport_main.so is loaded before the
 * dlopen()'d image, so the image's `_exit` reference binds to this symbol; the
 * real implementation is fetched with dlsym(RTLD_NEXT). */
void _exit(int status) __attribute__((noreturn));
void _exit(int status) {
    if (!g_in_exit) {
        g_in_exit = 1;
        drain_log_pipe(400);
    }
    if (!g_real_exit) g_real_exit = (fn_exit)dlsym(RTLD_NEXT, "_exit");
    if (g_real_exit) g_real_exit(status);
    /* Last resort: never hang on the way out if RTLD_NEXT did not resolve. */
    syscall(__NR_exit_group, status);
    for (;;) pause();
}

void _Exit(int status) __attribute__((noreturn));
void _Exit(int status) {
    _exit(status);
}

/* ------------------------------------------------------------------ */
/* static image                                                       */
/* ------------------------------------------------------------------ */

static void *open_static_image(const char *lib_dir) {
    void *h = dlopen(STATIC_LIB_NAME, RTLD_NOW | RTLD_LOCAL);
    if (h) {
        LOGI("loaded %s via the default linker search path", STATIC_LIB_NAME);
        return h;
    }
    const char *first_error = dlerror();
    LOGI("dlopen(%s) failed: %s", STATIC_LIB_NAME,
         first_error ? first_error : "(no dlerror)");
    if (lib_dir && *lib_dir) {
        char path[PATH_MAX];
        if (join_path(path, sizeof(path), lib_dir, STATIC_LIB_NAME) == 0) {
            h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            if (h) {
                LOGI("loaded %s from %s", STATIC_LIB_NAME, path);
                return h;
            }
            const char *second_error = dlerror();
            LOGI("dlopen(%s) failed: %s", path,
                 second_error ? second_error : "(no dlerror)");
        }
    }
    return NULL;
}

static void *sym_opt(void *h, const char *name) {
    dlerror();
    void *p = dlsym(h, name);
    const char *e = dlerror();
    if (!p || e) {
        LOGI("optional symbol %s missing%s%s", name, e ? ": " : "",
             e ? e : "");
        return NULL;
    }
    return p;
}

static int resolve_api(void *h) {
    memset(&g_host.api, 0, sizeof(g_host.api));

    dlerror();
    g_host.api.entry = (fn_entry)dlsym(h, PORT_SYM_ENTRY);
    const char *e = dlerror();
    if (!g_host.api.entry || e) {
        LOGE("static image has no usable %s (%s); refusing to continue",
             PORT_SYM_ENTRY, e ? e : "symbol resolved to NULL");
        return -1;
    }
    LOGI("resolved mandatory symbol %s", PORT_SYM_ENTRY);

    g_host.api.entry_ex = (fn_entry_ex)sym_opt(h, PORT_SYM_ENTRY_EX);
    g_host.api.set_native_window =
        (fn_set_native_window)sym_opt(h, PORT_SYM_SET_NATIVE_WINDOW);
    g_host.api.set_surface_size =
        (fn_set_surface_size)sym_opt(h, PORT_SYM_SET_SURFACE_SIZE);
    g_host.api.set_gamepad_state =
        (fn_set_gamepad_state)sym_opt(h, PORT_SYM_SET_GAMEPAD_STATE);
    g_host.api.post_host_input =
        (fn_post_host_input)sym_opt(h, PORT_SYM_POST_HOST_INPUT);
    g_host.api.on_lifecycle = (fn_on_lifecycle)sym_opt(h, PORT_SYM_ON_LIFECYCLE);
    g_host.api.on_pause = (fn_void)sym_opt(h, PORT_SYM_ON_PAUSE);
    g_host.api.on_resume = (fn_void)sym_opt(h, PORT_SYM_ON_RESUME);
    g_host.api.on_window_destroy =
        (fn_void)sym_opt(h, PORT_SYM_ON_WINDOW_DESTROY);

    /* runtime/guest_user32.h fallback: the handle lives in the image's
     * exported data symbol g_user32 (a pointer variable). */
    g_host.api.gu_set_surface_size =
        (fn_gu_set_surface_size)sym_opt(h, PORT_SYM_GU_SET_SURFACE_SIZE);
    g_host.api.gu_post_host_input =
        (fn_gu_post_host_input)sym_opt(h, PORT_SYM_GU_POST_HOST_INPUT);
    if (g_host.api.gu_set_surface_size || g_host.api.gu_post_host_input) {
        g_host.api.gu_user32 = (void **)sym_opt(h, PORT_SYM_GU_USER32_DATA);
        if (!g_host.api.gu_user32) {
            LOGW("gu_* bridges are exported but the %s data symbol is not; "
                 "the user32 surface/input path stays inactive",
                 PORT_SYM_GU_USER32_DATA);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* surface / window forwarding                                        */
/* ------------------------------------------------------------------ */

static void *gu_user32_handle(void) {
    if (!g_host.api.gu_user32) return NULL;
    return *g_host.api.gu_user32;
}

static void forward_surface_size(int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    g_host.width = w;
    g_host.height = h;
    if (g_host.api.set_surface_size) {
        int rc = g_host.api.set_surface_size(w, h);
        LOGI("%s(%d, %d) -> %d", PORT_SYM_SET_SURFACE_SIZE, w, h, rc);
        return;
    }
    if (g_host.api.gu_set_surface_size) {
        void *u32 = gu_user32_handle();
        if (u32) {
            int rc = g_host.api.gu_set_surface_size(u32, (uint32_t)w, (uint32_t)h);
            LOGI("%s(%d, %d) -> %d", PORT_SYM_GU_SET_SURFACE_SIZE, w, h, rc);
            return;
        }
    }
    LOGI("surface size %dx%d recorded locally; the image exports no "
         "port_set_surface_size/gu_set_surface_size sink", w, h);
}

static int forward_native_window(ANativeWindow *window, int32_t w, int32_t h) {
    g_host.window = window;
    if (!g_host.api.set_native_window) return 0;
    int rc = g_host.api.set_native_window(window, w, h);
    LOGI("%s(%p, %d, %d) -> %d", PORT_SYM_SET_NATIVE_WINDOW, (void *)window, w, h,
         rc);
    return rc == 0;
}

static void forward_lifecycle(uint32_t event, void *data) {
    if (g_host.api.on_lifecycle) {
        int rc = g_host.api.on_lifecycle(event, data);
        LOGI("port_on_lifecycle(%u) -> %d", event, rc);
        return;
    }
    switch (event) {
    case PORT_LIFECYCLE_PAUSE:
        if (g_host.api.on_pause) g_host.api.on_pause();
        break;
    case PORT_LIFECYCLE_RESUME:
        if (g_host.api.on_resume) g_host.api.on_resume();
        break;
    case PORT_LIFECYCLE_WINDOW_DESTROYED:
        if (g_host.api.on_window_destroy) g_host.api.on_window_destroy();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

static int post_input(uint32_t type, uint32_t x, uint32_t y, uint32_t code) {
    if (g_host.api.post_host_input) {
        return g_host.api.post_host_input(type, x, y, code);
    }
    if (g_host.api.gu_post_host_input) {
        void *u32 = gu_user32_handle();
        if (u32) {
            port_gu_host_input in;
            memset(&in, 0, sizeof(in));
            in.type = type;
            in.x = x;
            in.y = y;
            in.code = code;
            in.width = (uint32_t)g_host.width;
            in.height = (uint32_t)g_host.height;
            return g_host.api.gu_post_host_input(u32, &in);
        }
    }
    if (!g_host.input_sink_missing_logged) {
        g_host.input_sink_missing_logged = 1;
        LOGW("no input sink in the image (port_post_host_input / "
             "gu_post_host_input+g_user32); host input is logged and dropped");
    }
    return -1;
}

static void push_gamepad_state(void) {
    if (!g_host.api.set_gamepad_state) return;
    port_gamepad_state st = g_host.pad;
    st.abi_version = PORT_HOST_API_ABI;
    st.connected = (uint32_t)(g_host.pad_connected ? 1 : 0);
    st.axis_count = PORT_AXIS_COUNT;
    int rc = g_host.api.set_gamepad_state(&st);
    if (rc != 0) LOGW("port_set_gamepad_state -> %d", rc);
}

static void pad_set_button(int bit, int down) {
    uint32_t mask = 1u << (uint32_t)bit;
    if (down) {
        g_host.pad.buttons |= mask;
    } else {
        g_host.pad.buttons &= ~mask;
    }
    if (bit >= PORT_PAD_DPAD_UP && bit <= PORT_PAD_DPAD_RIGHT) {
        g_host.pad.dpad = g_host.pad.buttons & 0x0Fu;
    }
}

static void pad_set_axis(int axis, float value) {
    if (axis < 0 || axis >= PORT_AXIS_COUNT) return;
    g_host.pad.axis[axis] = value;
}

/* Controller state that can arrive two ways: the d-pad as hat axes or as
 * DPAD keys, the triggers as analog axes or as digital L2/R2 buttons. Both
 * views are kept and merged, so neither clears the other. */
static uint32_t g_pad_key_dpad, g_pad_hat_dpad;
static int g_pad_key_lt, g_pad_key_rt;
static float g_pad_axis_lt, g_pad_axis_rt;
static int g_pad_analog_triggers;   /* the pad has reported an analog trigger value */

static void pad_apply_dpad(void) {
    uint32_t dpad = (g_pad_key_dpad | g_pad_hat_dpad) & 0x0Fu;
    g_host.pad.buttons = (g_host.pad.buttons & ~0x0Fu) | dpad;
    g_host.pad.dpad = dpad;
}

static void pad_apply_triggers(void) {
    /* A pad with analog triggers also sends L2/R2 as keys part-way through the
     * travel; those keys must not jump the trigger to full. Only a pad that
     * never reports an analog value has purely digital triggers. */
    if (g_pad_axis_lt > 0.0f || g_pad_axis_rt > 0.0f) g_pad_analog_triggers = 1;
    if (g_pad_analog_triggers) {
        pad_set_axis(PORT_AXIS_LEFT_TRIGGER, g_pad_axis_lt);
        pad_set_axis(PORT_AXIS_RIGHT_TRIGGER, g_pad_axis_rt);
    } else {
        pad_set_axis(PORT_AXIS_LEFT_TRIGGER, g_pad_key_lt ? 1.0f : 0.0f);
        pad_set_axis(PORT_AXIS_RIGHT_TRIGGER, g_pad_key_rt ? 1.0f : 0.0f);
    }
}

/* Android keycode -> Win32 virtual-key code. Returns 0 when unmapped (the
 * event is then only logged, never invented). */
static uint32_t android_key_to_vk(int32_t code, int *pad_button) {
    *pad_button = -1;
    switch (code) {
    case AKEYCODE_BACK: return PORT_VK_ESCAPE;
    case AKEYCODE_ESCAPE: return PORT_VK_ESCAPE;
    case AKEYCODE_ENTER:
    case AKEYCODE_NUMPAD_ENTER: return PORT_VK_RETURN;
    case AKEYCODE_TAB: return PORT_VK_TAB;
    case AKEYCODE_SPACE: return PORT_VK_SPACE;
    case AKEYCODE_DEL: return PORT_VK_BACK;
    case AKEYCODE_FORWARD_DEL: return PORT_VK_DELETE;
    case AKEYCODE_MOVE_HOME: return PORT_VK_HOME;
    case AKEYCODE_MOVE_END: return PORT_VK_END;
    case AKEYCODE_PAGE_UP: return PORT_VK_PRIOR;
    case AKEYCODE_PAGE_DOWN: return PORT_VK_NEXT;
    case AKEYCODE_INSERT: return PORT_VK_INSERT;
    case AKEYCODE_SHIFT_LEFT:
    case AKEYCODE_SHIFT_RIGHT: return PORT_VK_SHIFT;
    case AKEYCODE_CTRL_LEFT:
    case AKEYCODE_CTRL_RIGHT: return PORT_VK_CONTROL;
    case AKEYCODE_ALT_LEFT:
    case AKEYCODE_ALT_RIGHT: return PORT_VK_MENU;
    case AKEYCODE_DPAD_UP: return PORT_VK_UP;
    case AKEYCODE_DPAD_DOWN: return PORT_VK_DOWN;
    case AKEYCODE_DPAD_LEFT: return PORT_VK_LEFT;
    case AKEYCODE_DPAD_RIGHT: return PORT_VK_RIGHT;
    /* --- gamepad buttons: XInput VK_PAD_* range --- */
    case AKEYCODE_BUTTON_A: *pad_button = PORT_PAD_A; return PORT_VK_PAD_A;
    case AKEYCODE_BUTTON_B: *pad_button = PORT_PAD_B; return PORT_VK_PAD_B;
    case AKEYCODE_BUTTON_X: *pad_button = PORT_PAD_X; return PORT_VK_PAD_X;
    case AKEYCODE_BUTTON_Y: *pad_button = PORT_PAD_Y; return PORT_VK_PAD_Y;
    case AKEYCODE_BUTTON_L1:
        *pad_button = PORT_PAD_LEFT_SHOULDER; return PORT_VK_PAD_LEFT_SHOULDER;
    case AKEYCODE_BUTTON_R1:
        *pad_button = PORT_PAD_RIGHT_SHOULDER; return PORT_VK_PAD_RIGHT_SHOULDER;
    case AKEYCODE_BUTTON_L2: return PORT_VK_PAD_LEFT_TRIGGER;
    case AKEYCODE_BUTTON_R2: return PORT_VK_PAD_RIGHT_TRIGGER;
    case AKEYCODE_BUTTON_THUMBL:
        *pad_button = PORT_PAD_LEFT_THUMB; return PORT_VK_PAD_LEFT_THUMB;
    case AKEYCODE_BUTTON_THUMBR:
        *pad_button = PORT_PAD_RIGHT_THUMB; return PORT_VK_PAD_RIGHT_THUMB;
    case AKEYCODE_BUTTON_START:
        *pad_button = PORT_PAD_START; return PORT_VK_PAD_START;
    case AKEYCODE_BUTTON_SELECT:
        *pad_button = PORT_PAD_BACK; return PORT_VK_PAD_BACK;
    case AKEYCODE_DPAD_CENTER: return PORT_VK_PAD_A;
    default: break;
    }
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
        return (uint32_t)('A' + (code - AKEYCODE_A));
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
        return (uint32_t)('0' + (code - AKEYCODE_0));
    /* F1..F12 (VK_F1 = 0x70): the pause menu pages switch with F1/F2. */
    if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12)
        return (uint32_t)(0x70 + (code - AKEYCODE_F1));
    return 0;
}

/* Returns 1 when the event was consumed, 0 when the system should still see it
 * (volume keys, media keys, ... are not mapped and stay with Android). */
static int handle_key_event(AInputEvent *event) {
    int32_t code = AKeyEvent_getKeyCode(event);
    int32_t action = AKeyEvent_getAction(event);
    int repeat = (action == AKEY_EVENT_ACTION_MULTIPLE);
    int down = (action == AKEY_EVENT_ACTION_DOWN) || repeat;

    int pad_button = -1;
    uint32_t vk = android_key_to_vk(code, &pad_button);
    /* Keys that come from a game controller are controller state, not
     * keyboard keys: its d-pad (when it is reported as keys instead of the
     * hat axes) and the digital trigger buttons. */
    int32_t source = AInputEvent_getSource(event);
    int from_pad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                   (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
    if (from_pad) {
        int dpad = -1;
        switch (code) {
        case AKEYCODE_DPAD_UP: dpad = PORT_PAD_DPAD_UP; break;
        case AKEYCODE_DPAD_DOWN: dpad = PORT_PAD_DPAD_DOWN; break;
        case AKEYCODE_DPAD_LEFT: dpad = PORT_PAD_DPAD_LEFT; break;
        case AKEYCODE_DPAD_RIGHT: dpad = PORT_PAD_DPAD_RIGHT; break;
        default: break;
        }
        if (dpad >= 0) {
            g_host.pad_connected = 1;
            g_pad_key_dpad = down ? (g_pad_key_dpad | (1u << dpad)) : (g_pad_key_dpad & ~(1u << dpad));
            pad_apply_dpad();
            push_gamepad_state();
            return 1;
        }
        if (code == AKEYCODE_BUTTON_L2 || code == AKEYCODE_BUTTON_R2) {
            g_host.pad_connected = 1;
            if (code == AKEYCODE_BUTTON_L2) g_pad_key_lt = down; else g_pad_key_rt = down;
            pad_apply_triggers();
            push_gamepad_state();
            return 1;
        }
    }
    if (vk == 0) {
        LOGI("unmapped keycode %d (%s) left to the system", code,
             down ? "down" : "up");
        return 0;
    }
    if (pad_button >= 0) {
        g_host.pad_connected = 1;
        pad_set_button(pad_button, down);
    }
    int rc = post_input(down ? PORT_GU_INPUT_KEY_DOWN : PORT_GU_INPUT_KEY_UP,
                        0, 0, vk);
    LOGI("key %d -> VK 0x%02X %s (post -> %d)", code, vk,
         down ? "down" : "up", rc);
    if (pad_button >= 0) push_gamepad_state();
    if (vk == PORT_VK_ESCAPE && !down && rc < 0) {
        /* No input sink at all: keep the shell escapable. */
        LOGI("back/escape with no input sink -> finishing the activity");
        ANativeActivity_finish(g_host.app->activity);
    }
    return 1;
}

static void handle_motion_event(AInputEvent *event) {
    int32_t source = AInputEvent_getSource(event);
    /* Joystick-class events carry the controller axes; their X/Y are the left
     * stick, not a screen position, so they never become touches. */
    if ((source & AINPUT_SOURCE_CLASS_JOYSTICK) != 0) {
        if (!g_host.api.set_gamepad_state) return;
        float ax = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
        float ay = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
        float az = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
        float arz = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
        /* Triggers: LTRIGGER/RTRIGGER, or BRAKE/GAS on pads that use those
         * (the Odin 3 pad reports both pairs). */
        float lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0);
        float rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0);
        float brake = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        float gas = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        if (brake > lt) lt = brake;
        if (gas > rt) rt = gas;

        pad_set_axis(PORT_AXIS_LEFT_X, ax);
        pad_set_axis(PORT_AXIS_LEFT_Y, ay);
        pad_set_axis(PORT_AXIS_RIGHT_X, az);
        pad_set_axis(PORT_AXIS_RIGHT_Y, arz);
        g_pad_axis_lt = lt;
        g_pad_axis_rt = rt;
        pad_apply_triggers();
        pad_set_axis(PORT_AXIS_HAT_X, hx);
        pad_set_axis(PORT_AXIS_HAT_Y, hy);

        /* Hat axes are the d-pad: fold them into the button mask too. */
        g_pad_hat_dpad = (hx < -0.5f ? 1u << PORT_PAD_DPAD_LEFT : 0u) | (hx > 0.5f ? 1u << PORT_PAD_DPAD_RIGHT : 0u) |
                         (hy < -0.5f ? 1u << PORT_PAD_DPAD_UP : 0u) | (hy > 0.5f ? 1u << PORT_PAD_DPAD_DOWN : 0u);
        pad_apply_dpad();

        g_host.pad_connected = 1;
        push_gamepad_state();
        return;
    }
    if ((source & AINPUT_SOURCE_CLASS_POINTER) == 0) return;

    int32_t action = AMotionEvent_getAction(event);
    int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    size_t pointers = AMotionEvent_getPointerCount(event);
    float x = AMotionEvent_getX(event, 0);
    float y = AMotionEvent_getY(event, 0);

    if (pointers > 1 && !g_host.pointer_extra_logged) {
        g_host.pointer_extra_logged = 1;
        LOGI("multi-touch: %zu pointers, only pointer 0 is forwarded", pointers);
    }

    switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        post_input(PORT_GU_INPUT_TOUCH_DOWN, (uint32_t)x, (uint32_t)y, 0);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
    case AMOTION_EVENT_ACTION_CANCEL:
        post_input(PORT_GU_INPUT_TOUCH_UP, (uint32_t)x, (uint32_t)y, 0);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        post_input(PORT_GU_INPUT_TOUCH_MOVE, (uint32_t)x, (uint32_t)y, 0);
        break;
    default:
        break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    switch (AInputEvent_getType(event)) {
    case AINPUT_EVENT_TYPE_KEY:
        return handle_key_event(event);
    case AINPUT_EVENT_TYPE_MOTION:
        handle_motion_event(event);
        return 1;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* EGL heartbeat (only while the image does not own the window)       */
/* ------------------------------------------------------------------ */

static int engine_init_display(void) {
    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };
    EGLint numConfigs = 0;
    EGLConfig config = NULL;
    EGLint format = 0;
    EGLint w = 0, h = 0;

    g_host.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_host.display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return -1;
    }
    if (!eglInitialize(g_host.display, NULL, NULL)) {
        LOGE("eglInitialize failed (0x%x)", eglGetError());
        g_host.display = EGL_NO_DISPLAY;
        return -1;
    }
    if (!eglChooseConfig(g_host.display, attribs, &config, 1, &numConfigs) ||
        numConfigs < 1) {
        LOGE("eglChooseConfig failed (0x%x)", eglGetError());
        return -1;
    }
    if (!eglGetConfigAttrib(g_host.display, config, EGL_NATIVE_VISUAL_ID, &format)) {
        LOGE("eglGetConfigAttrib failed (0x%x)", eglGetError());
        return -1;
    }
    ANativeWindow_setBuffersGeometry(g_host.app->window, 0, 0, format);
    g_host.surface = eglCreateWindowSurface(g_host.display, config,
                                            g_host.app->window, NULL);
    if (g_host.surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed (0x%x)", eglGetError());
        return -1;
    }
    const EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_host.context = eglCreateContext(g_host.display, config, EGL_NO_CONTEXT,
                                      ctx_attribs);
    if (g_host.context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (0x%x)", eglGetError());
        return -1;
    }
    if (!eglMakeCurrent(g_host.display, g_host.surface, g_host.surface,
                        g_host.context)) {
        LOGE("eglMakeCurrent failed (0x%x)", eglGetError());
        return -1;
    }
    if (!eglQuerySurface(g_host.display, g_host.surface, EGL_WIDTH, &w) ||
        !eglQuerySurface(g_host.display, g_host.surface, EGL_HEIGHT, &h)) {
        LOGE("eglQuerySurface failed (0x%x)", eglGetError());
        return -1;
    }
    g_host.width = w;
    g_host.height = h;
    g_host.animating = 1;
    g_host.heartbeat_enabled = 1;
    LOGI("EGL heartbeat ready %dx%d (GL_RENDERER=%s)", w, h,
         (const char *)glGetString(GL_RENDERER));
    return 0;
}

static void engine_term_display(void) {
    g_host.animating = 0;
    g_host.heartbeat_enabled = 0;
    if (g_host.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_host.display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (g_host.context != EGL_NO_CONTEXT)
            eglDestroyContext(g_host.display, g_host.context);
        if (g_host.surface != EGL_NO_SURFACE)
            eglDestroySurface(g_host.display, g_host.surface);
        eglTerminate(g_host.display);
    }
    g_host.display = EGL_NO_DISPLAY;
    g_host.context = EGL_NO_CONTEXT;
    g_host.surface = EGL_NO_SURFACE;
}

static void engine_draw_frame(void) {
    if (g_host.display == EGL_NO_DISPLAY || !g_host.animating) return;

    float t = (float)(g_host.frames % 240) / 240.0f;
    float pulse = 0.5f - 0.5f * (float)cos(t * 6.2831853f);
    glViewport(0, 0, g_host.width, g_host.height);
    glClearColor(0.02f + 0.10f * pulse, 0.03f + 0.18f * pulse,
                 0.06f + 0.30f * pulse, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!eglSwapBuffers(g_host.display, g_host.surface)) {
        LOGE("eglSwapBuffers failed (0x%x)", eglGetError());
        engine_term_display();
        return;
    }
    g_host.frames++;
    if (g_host.frames % 600 == 1) {
        LOGI("shell alive: %lld frames, %dx%d, game thread %s",
             (long long)g_host.frames, g_host.width, g_host.height,
             g_host.thread_done ? "finished" : "running");
    }
}

/* ------------------------------------------------------------------ */
/* game thread                                                        */
/* ------------------------------------------------------------------ */

static void *game_thread_main(void *arg) {
    (void)arg;
    g_host.thread_running = 1;
    LOGI("game thread start (tid %d): %s(\"%s\", \"%s\")", gettid(),
         PORT_SYM_ENTRY, g_host.game_root, g_host.user_root);
    int rc;
    if (g_host.api.entry_ex) {
        /* No NLS bytes are shipped with the APK: the image must stop honestly
         * on the ANSI code page paths rather than guess a table. */
        rc = g_host.api.entry_ex(g_host.game_root, g_host.user_root, NULL);
    } else {
        rc = g_host.api.entry(g_host.game_root, g_host.user_root);
    }
    g_host.thread_rc = rc;
    g_host.thread_running = 0;
    g_host.thread_done = 1;
    LOGI("game thread finished: port_entry returned %d", rc);
    return NULL;
}

/* The app language (Android 13+: Settings -> Apps -> Burnout -> Language,
 * offered from res/xml/locales_config.xml; otherwise the system language)
 * goes to the image as PORT_SYSTEM_LANGUAGE="de-DE". The image turns it into
 * the language setting the game's installer would have written. */
static void publish_app_language(struct android_app *app) {
    char language[3] = {0}, country[3] = {0}, value[16];
    if (!app || !app->config) return;
    AConfiguration_getLanguage(app->config, language);
    AConfiguration_getCountry(app->config, country);
    if (!language[0]) return;
    if (country[0]) snprintf(value, sizeof value, "%c%c-%c%c", language[0], language[1], country[0], country[1]);
    else snprintf(value, sizeof value, "%c%c", language[0], language[1]);
    setenv("PORT_SYSTEM_LANGUAGE", value, 1);
    LOGI("app language %s", value);
}

static int start_game_thread(void) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    int rc = pthread_attr_setstacksize(&attr, PORT_GAME_THREAD_STACK);
    if (rc != 0) {
        LOGE("pthread_attr_setstacksize(%u) failed: %s",
             (unsigned)PORT_GAME_THREAD_STACK, strerror(rc));
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_setguardsize(&attr, 64 * 1024);
    rc = pthread_create(&g_host.thread, &attr, game_thread_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        LOGE("pthread_create(game) failed: %s", strerror(rc));
        return -1;
    }
    g_host.thread_started = 1;
    pthread_setname_np(g_host.thread, "burnout-game");
    LOGI("game thread started with a %u MiB host stack",
         (unsigned)(PORT_GAME_THREAD_STACK >> 20));
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* The game renders 60 frames per second (its own limit; the image paces
 * Present to 60 Hz). ANativeWindow_setFrameRate (API 30) tells the compositor,
 * so a 120 Hz panel shows every frame twice in an even rhythm and an adaptive
 * setting may drop to 60 Hz. Best effort: missing on older Android. */
static void announce_frame_rate(ANativeWindow *window) {
    typedef int32_t (*fn_set_frame_rate)(ANativeWindow *, float, int8_t);
    static fn_set_frame_rate set_frame_rate;
    static int looked_up;
    if (!looked_up) {
        set_frame_rate = (fn_set_frame_rate)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
        looked_up = 1;
    }
    if (!window || !set_frame_rate) return;
    int32_t rc = set_frame_rate(window, 60.0f, 1 /* ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE */);
    LOGI("ANativeWindow_setFrameRate(60, fixed source) -> %d", rc);
}

static void engine_handle_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_SAVE_STATE:
        break;
    case APP_CMD_INIT_WINDOW: {
        if (app->window == NULL) break;
        int32_t w = ANativeWindow_getWidth(app->window);
        int32_t h = ANativeWindow_getHeight(app->window);
        LOGI("window created %dx%d", w, h);
        announce_frame_rate(app->window);
        forward_surface_size(w, h);
        g_host.window_owned_by_image = forward_native_window(app->window, w, h);
        if (g_host.window_owned_by_image) {
            LOGI("image owns the window; the EGL heartbeat stays off");
        } else if (engine_init_display() == 0) {
            engine_draw_frame();
        }
        forward_lifecycle(PORT_LIFECYCLE_WINDOW_CREATED, app->window);
        break;
    }
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED: {
        if (app->window == NULL) break;
        int32_t w = ANativeWindow_getWidth(app->window);
        int32_t h = ANativeWindow_getHeight(app->window);
        LOGI("window resized %dx%d", w, h);
        forward_surface_size(w, h);
        post_input(PORT_GU_INPUT_RESIZE, 0, 0, 0);
        break;
    }
    case APP_CMD_TERM_WINDOW:
        LOGI("window destroyed");
        forward_native_window(NULL, 0, 0);
        forward_lifecycle(PORT_LIFECYCLE_WINDOW_DESTROYED, NULL);
        engine_term_display();
        break;
    case APP_CMD_GAINED_FOCUS:
        g_host.animating = 1;
        LOGI("gained focus");
        post_input(PORT_GU_INPUT_FOCUS_GAINED, 0, 0, 0);
        forward_lifecycle(PORT_LIFECYCLE_FOCUS_GAINED, NULL);
        break;
    case APP_CMD_LOST_FOCUS:
        g_host.animating = 0;
        LOGI("lost focus");
        post_input(PORT_GU_INPUT_FOCUS_LOST, 0, 0, 0);
        forward_lifecycle(PORT_LIFECYCLE_FOCUS_LOST, NULL);
        break;
    case APP_CMD_PAUSE:
        LOGI("pause");
        forward_lifecycle(PORT_LIFECYCLE_PAUSE, NULL);
        break;
    case APP_CMD_RESUME:
        LOGI("resume");
        forward_lifecycle(PORT_LIFECYCLE_RESUME, NULL);
        break;
    case APP_CMD_LOW_MEMORY:
        LOGW("low memory notification");
        break;
    case APP_CMD_DESTROY:
        LOGI("APP_CMD_DESTROY");
        forward_lifecycle(PORT_LIFECYCLE_DESTROY, NULL);
        forward_native_window(NULL, 0, 0);
        engine_term_display();
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* paths, address-space probe, immersive mode                         */
/* ------------------------------------------------------------------ */

/* If the previous run died before its trap line reached port-last-trap.txt
 * (killed process, interposition not bound, ...), the runtime log mirror still
 * has it: copy the PORT_TRAP lines over on the next start. */
static void recover_previous_trap(void) {
    struct stat st;
    if (g_host.trap_path[0] && stat(g_host.trap_path, &st) == 0 && st.st_size > 0)
        return; /* the current trap file already has content */
    FILE *f = fopen(g_host.log_path, "re");
    if (!f) return;
    char line[2048];
    int recovered = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PORT_TRAP", 9) != 0) continue;
        if (!recovered) {
            trap_file_append("[recovered from the previous run's log mirror]");
            recovered = 1;
        }
        trap_file_append(line);
    }
    fclose(f);
    if (recovered) LOGW("recovered a PORT_TRAP line from %s", g_host.log_path);
}

static void build_paths(void) {
    const char *ext = NULL;
    const char *in = NULL;
    if (g_host.app->activity) {
        ext = g_host.app->activity->externalDataPath;
        in = g_host.app->activity->internalDataPath;
    }
    if (ext && *ext) {
        snprintf(g_host.files_dir, sizeof(g_host.files_dir), "%s", ext);
    } else {
        snprintf(g_host.files_dir, sizeof(g_host.files_dir), "%s", FILES_FALLBACK);
        LOGW("externalDataPath unavailable; assuming %s", g_host.files_dir);
    }
    if (join_path(g_host.game_root, sizeof(g_host.game_root), g_host.files_dir,
                  "BurnoutPR") != 0) {
        snprintf(g_host.game_root, sizeof(g_host.game_root), "%s",
                 GAME_DATA_FALLBACK);
    }
    if (in && *in) {
        if (join_path(g_host.user_root, sizeof(g_host.user_root), in, "user") != 0)
            snprintf(g_host.user_root, sizeof(g_host.user_root), "%s/user",
                     g_host.files_dir);
    } else {
        if (join_path(g_host.user_root, sizeof(g_host.user_root),
                      g_host.files_dir, "user") != 0)
            snprintf(g_host.user_root, sizeof(g_host.user_root), "%s/user",
                     g_host.files_dir);
    }
    join_path(g_host.trap_path, sizeof(g_host.trap_path), g_host.files_dir,
              "port-last-trap.txt");
    join_path(g_host.log_path, sizeof(g_host.log_path), g_host.files_dir,
              "port-runtime.log");

    LOGI("files dir   : %s", g_host.files_dir);
    LOGI("game root   : %s", g_host.game_root);
    LOGI("user root   : %s", g_host.user_root);

    if (mkdir_p(g_host.user_root) != 0) {
        LOGW("cannot create user root %s: %s", g_host.user_root, strerror(errno));
    }
    struct stat st;
    if (stat(g_host.game_root, &st) == 0 && S_ISDIR(st.st_mode)) {
        LOGI("game data present: %s", g_host.game_root);
    } else {
        LOGW("game data missing: %s (the user has to copy the game files there)",
             g_host.game_root);
    }
    /* port-last-trap.txt is append-only (one timestamped block per trap): a
     * trap that was lost in the previous run is recovered from the log mirror
     * before this run can add its own. */
    recover_previous_trap();
    int fd = open(g_host.trap_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0660);
    if (fd >= 0) close(fd);
}

static int probe_one_mapping(const char *label, int prot) {
    errno = 0;
    void *p = mmap(NULL, GUEST_FLAT_SIZE, prot,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOGW("4 GiB %s|MAP_NORESERVE reservation FAILED: %s", label,
             strerror(errno));
        return -1;
    }
    LOGI("4 GiB %s|MAP_NORESERVE reservation OK at %p (no RAM committed)", label,
         p);
    munmap(p, GUEST_FLAT_SIZE);
    return 0;
}

/* runtime/static_entry.c flat_reserve() uses
 *   mmap(4 GiB, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_NORESERVE)
 * which is exactly the pattern Android 10+ refuses for apps with
 * targetSdkVersion >= 29 (SELinux `execmem` is not granted to
 * untrusted_app_29+). All three variants are probed here so the first device
 * run answers the question instead of guessing:
 *   RWX  = what the runtime does today
 *   RW   = the same reservation without PROT_EXEC (the lifted code is compiled
 *          C, so guest pages are never executed)
 *   NONE = pure address-space reservation
 */
static void probe_guest_address_space(void) {
    int rwx = probe_one_mapping("PROT_READ|PROT_WRITE|PROT_EXEC",
                                PROT_READ | PROT_WRITE | PROT_EXEC);
    int rw = probe_one_mapping("PROT_READ|PROT_WRITE", PROT_READ | PROT_WRITE);
    int none = probe_one_mapping("PROT_NONE", PROT_NONE);
    if (rwx == 0) {
        LOGI("guest flat space: RWX reservation allowed on this device");
    } else if (rw == 0) {
        LOGW("guest flat space: RWX is refused but RW works -- the runtime's "
             "flat_reserve() needs PROT_READ|PROT_WRITE instead of RWX");
    } else if (none == 0) {
        LOGW("guest flat space: only PROT_NONE works -- the runtime's "
             "flat_reserve() needs a PROT_NONE reservation plus mprotect");
    } else {
        LOGE("guest flat space: no 4 GiB reservation possible at all");
    }
}

/* Immersive sticky fullscreen. Best effort: any JNI problem is logged and
 * cleared, the activity keeps running. */
static void set_immersive_mode(void) {
    struct android_app *app = g_host.app;
    if (!app->activity || !app->activity->vm || !app->activity->clazz) {
        LOGW("no activity/vm; immersive mode not applied");
        return;
    }
    JNIEnv *env = NULL;
    if ((*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL) !=
            0 ||
        !env) {
        LOGW("AttachCurrentThread failed; immersive mode not applied");
        return;
    }
#define JNI_CLEAR(what)                                                        \
    do {                                                                       \
        if ((*env)->ExceptionCheck(env)) {                                     \
            (*env)->ExceptionDescribe(env);                                    \
            (*env)->ExceptionClear(env);                                       \
            LOGW("JNI failure while %s; immersive mode not applied", what);    \
            return;                                                            \
        }                                                                      \
    } while (0)

    jclass act_cls = (*env)->GetObjectClass(env, app->activity->clazz);
    JNI_CLEAR("resolving the activity class");
    jmethodID get_window = (*env)->GetMethodID(env, act_cls, "getWindow",
                                               "()Landroid/view/Window;");
    JNI_CLEAR("resolving Activity.getWindow");
    jobject window = (*env)->CallObjectMethod(env, app->activity->clazz, get_window);
    JNI_CLEAR("calling Activity.getWindow");
    jclass win_cls = (*env)->GetObjectClass(env, window);
    jmethodID get_decor = (*env)->GetMethodID(env, win_cls, "getDecorView",
                                              "()Landroid/view/View;");
    JNI_CLEAR("resolving Window.getDecorView");
    jobject decor = (*env)->CallObjectMethod(env, window, get_decor);
    JNI_CLEAR("calling Window.getDecorView");
    jclass view_cls = (*env)->GetObjectClass(env, decor);
    jmethodID set_vis = (*env)->GetMethodID(env, view_cls, "setSystemUiVisibility",
                                            "(I)V");
    JNI_CLEAR("resolving View.setSystemUiVisibility");
    /* SYSTEM_UI_FLAG_LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION |
     * LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN | IMMERSIVE_STICKY */
    const jint flags = 0x100 | 0x200 | 0x400 | 0x2 | 0x4 | 0x1000;
    (*env)->CallVoidMethod(env, decor, set_vis, flags);
    JNI_CLEAR("calling View.setSystemUiVisibility");
#undef JNI_CLEAR
    LOGI("immersive sticky fullscreen requested (flags 0x%x)", (unsigned)flags);
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

/* Android may destroy the activity and create a new one while the process
 * (and with it the running game) stays alive: another app in front for long,
 * a configuration change the manifest does not take over, the task brought back
 * from the recents screen. The game thread, the static image and the log reader
 * belong to the process, not to the activity: a new activity attaches its
 * window to the game that is already running instead of starting a second one
 * (the guest address space, the image's globals and its threads exist once). */
static int g_game_in_process;

void android_main(struct android_app *app) {
    int reattach = g_game_in_process && g_host.thread_started && !g_host.thread_done;
    if (reattach) {
        /* Only the activity-bound part of the state starts over. */
        g_host.window = NULL;
        g_host.width = g_host.height = 0;
        g_host.window_owned_by_image = 0;
        g_host.animating = 0;
        g_host.frames = 0;
    } else {
        memset(&g_host, 0, sizeof(g_host));
    }
    g_host.app = app;
    g_host.display = EGL_NO_DISPLAY;
    g_host.surface = EGL_NO_SURFACE;
    g_host.context = EGL_NO_CONTEXT;

    app->userData = &g_host;
    app->onAppCmd = engine_handle_cmd;
    app->onInputEvent = handle_input;

    LOGI("burnoutport host start (pid %d, host-api abi=%u, sdk=%d)", getpid(),
         PORT_HOST_API_ABI, android_get_device_api_level());

    if (reattach) {
        LOGI("activity recreated in the running process (pid %d): the game keeps "
             "running, only the new window is attached", getpid());
    } else {
        build_paths();
        probe_guest_address_space();
        redirect_streams_to_logcat();
    }

    ANativeActivity_setWindowFlags(app->activity,
                                   AWINDOW_FLAG_FULLSCREEN |
                                       AWINDOW_FLAG_KEEP_SCREEN_ON,
                                   0);
    set_immersive_mode();

    char lib_dir[PATH_MAX] = {0};
    if (discover_lib_dir(lib_dir, sizeof(lib_dir))) {
        LOGI("native lib dir: %s", lib_dir);
    } else {
        LOGW("native lib dir unknown; relying on the default dlopen search path");
    }

    if (reattach) {
        /* image, API and game thread are already there */
    } else if (!(g_host.image = open_static_image(lib_dir))) {
        LOGI("static image missing: %s not present in the app lib dir (%s); "
             "running the EGL shell only", STATIC_LIB_NAME,
             lib_dir[0] ? lib_dir : "unknown");
    } else if (resolve_api(g_host.image) != 0) {
        LOGE("static image present but unusable; running the EGL shell only");
        g_host.image = NULL;
    } else if ((publish_app_language(app), start_game_thread() != 0)) {
        LOGE("game thread could not be started; running the EGL shell only");
    } else {
        g_game_in_process = 1;
    }

    while (1) {
        int events = 0;
        struct android_poll_source *source = NULL;

        /* Poll without waiting only while this shell draws its own heartbeat
         * frames. When the image owns the window there is nothing to draw
         * here, and a zero timeout made this loop spin a whole core for the
         * entire session (heat, and one of the two fast cores gone). */
        int heartbeat = g_host.animating && !g_host.window_owned_by_image &&
                        g_host.display != EGL_NO_DISPLAY;
        while (ALooper_pollOnce(heartbeat ? 0 : -1, NULL, &events,
                                (void **)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested != 0 && g_game_in_process && !g_host.thread_done) {
                /* The activity goes, the process and the game stay: keep the
                 * log reader (the game blocks on a full log pipe otherwise)
                 * and let a new activity attach its window (see above). */
                LOGI("activity destroyed; the game keeps running in the process "
                     "and waits for the next window");
                return;
            }
            if (app->destroyRequested != 0) {
                LOGI("destroy requested; waiting for the game thread");
                if (g_host.thread_started && !g_host.thread_done) {
                    struct timespec ts = {0, 200 * 1000 * 1000};
                    for (int i = 0; i < 10 && !g_host.thread_done; i++)
                        nanosleep(&ts, NULL);
                }
                engine_term_display();
                LOGI("host exit after %lld heartbeat frames (game thread %s, "
                     "rc %d)", (long long)g_host.frames,
                     g_host.thread_done ? "finished" : "still running",
                     g_host.thread_rc);
                drain_log_pipe(400);
                return;
            }
        }
        if (heartbeat) engine_draw_frame();
    }
}
