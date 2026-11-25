/* winmm bridges: multimedia timers and the waveOut path.
 * See guest_winmm.h for the modelled scope and the named limits. */
#if !defined(_POSIX_C_SOURCE) && !defined(__ANDROID__)
#define _POSIX_C_SOURCE 200809L /* clock_gettime/nanosleep on the host tests */
#endif
#include "guest_winmm.h"
#include "guest_handles.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int used;
    uint32_t id;
    uint32_t delay;
    uint32_t periodic;
    uint32_t guest_proc;
    uint32_t user;
    int stop;
    pthread_t thread;
    int thread_live;
} GwmTimer;

typedef struct {
    uint32_t header;   /* guest address of the WAVEHDR */
    uint32_t block_id; /* echoed by the backend on completion */
} GwmPendingBlock;

typedef struct {
    uint32_t backend_handle;
    uint32_t device;
    uint8_t format[GWM_WAVEFORMATEX_SIZE];
    uint32_t magic;
    /* A18: callback contract chosen by the guest in waveOutOpen. */
    uint32_t guest_handle;   /* HWAVEOUT as the guest sees it */
    uint32_t callback_type;  /* GWM_CALLBACK_* */
    uint32_t callback;       /* guest function address or event handle */
    uint32_t user;           /* dwInstance */
    GwmPendingBlock pending[GWM_MAX_PENDING_BLOCKS];
    uint32_t pending_count;
} GwmWaveOut;

#define GWM_WAVE_MAGIC 0x4d574747u /* "GWWM" */

struct GuestWinmm {
    GuestMemory *memory;
    uint32_t teb;
    const GwmAudioBackend *backend;
    void *backend_context;
    GwmGuestCall call;
    void *call_context;
    uint32_t period_count;
    uint32_t live_wave_out;
    uint32_t bytes_written;
    uint32_t timer_fires;
    uint32_t next_timer_id;
    GwmTimer timers[GWM_MAX_TIMERS];
    GwmEventSet event_set;
    void *event_context;
    GwmWaveOut *waves[GWM_MAX_WAVE_OUT];
    uint32_t next_block_id;
    uint32_t wave_done_count;
    uint32_t wave_callbacks;
    uint32_t wave_events;
    uint32_t wave_position;
    pthread_mutex_t lock;
    char last_stop[160];
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
static int write_u32(GuestWinmm *wmm, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(wmm->memory, address, b, 4);
}
static int stop(GuestWinmm *wmm, int code, const char *what) {
    snprintf(wmm->last_stop, sizeof(wmm->last_stop), "%s", what);
    fprintf(stderr, "winmm-stop: %s (%d)\n", what, code);
    return code;
}
static uint64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* --- null backend ------------------------------------------------------- */
static uint32_t null_bytes;
static int null_open(void *context, uint32_t device, const uint8_t *format,
                     uint32_t format_size, uint32_t *backend_handle) {
    (void)context;
    (void)format;
    (void)format_size;
    *backend_handle = device + 1u; /* unique per device id, never a pointer */
    return 0;
}
static int null_write(void *context, uint32_t backend_handle,
                      const uint8_t *data, uint32_t size) {
    (void)context;
    (void)backend_handle;
    (void)data;
    null_bytes += size;
    return 0;
}
static int null_reset(void *context, uint32_t backend_handle) {
    (void)context;
    (void)backend_handle;
    return 0;
}
static int null_close(void *context, uint32_t backend_handle) {
    (void)context;
    (void)backend_handle;
    return 0;
}
static const GwmAudioBackend null_backend = {
    "null", null_open, null_write, null_reset, null_close,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
const GwmAudioBackend *gwm_null_backend(void) { return &null_backend; }
uint32_t gwm_null_bytes(void) { return null_bytes; }

/* --- lifecycle ---------------------------------------------------------- */
int gwm_create(GuestMemory *memory, uint32_t teb, GuestWinmm **out) {
    if (!memory || !out) return GM_RANGE;
    GuestWinmm *wmm = (GuestWinmm *)calloc(1, sizeof(*wmm));
    if (!wmm) return GM_ALLOCATION;
    wmm->memory = memory;
    wmm->teb = teb;
    wmm->next_timer_id = 1;
    pthread_mutex_init(&wmm->lock, NULL);
    *out = wmm;
    return GM_OK;
}
static void wave_destroy(void *object) {
    GwmWaveOut *wave = (GwmWaveOut *)object;
    free(wave);
}
/* Stops and joins every timer thread; the callback is never invoked after
 * this returns. */
static void timers_stop_all(GuestWinmm *wmm) {
    for (uint32_t i = 0; i < GWM_MAX_TIMERS; i++) {
        GwmTimer *timer = &wmm->timers[i];
        if (!timer->used) continue;
        timer->stop = 1;
        if (timer->thread_live) {
            pthread_join(timer->thread, NULL);
            timer->thread_live = 0;
        }
        timer->used = 0;
    }
}
void gwm_destroy(GuestWinmm *wmm) {
    if (!wmm) return;
    timers_stop_all(wmm);
    pthread_mutex_destroy(&wmm->lock);
    free(wmm);
}
/* Forward declaration: the completion trampoline lives with the waveOut code. */
static void wave_block_done(void *context, uint32_t backend_handle,
                            uint32_t block_id);
int gwm_set_audio_backend(GuestWinmm *wmm, const GwmAudioBackend *backend,
                          void *context) {
    if (!wmm) return GM_UNSUPPORTED;
    wmm->backend = backend;
    wmm->backend_context = context;
    /* The asynchronous path needs the backend to report completions. A backend
     * without set_done keeps the synchronous a16 behaviour. */
    if (backend && backend->set_done)
        backend->set_done(context, wave_block_done, wmm);
    return GM_OK;
}
void gwm_set_event_hook(GuestWinmm *wmm, GwmEventSet set, void *context) {
    if (!wmm) return;
    wmm->event_set = set;
    wmm->event_context = context;
}
void gwm_set_guest_call(GuestWinmm *wmm, GwmGuestCall call, void *context) {
    if (!wmm) return;
    wmm->call = call;
    wmm->call_context = context;
}
uint32_t gwm_period_count(const GuestWinmm *wmm) {
    return wmm ? wmm->period_count : 0u;
}
uint32_t gwm_live_timers(const GuestWinmm *wmm) {
    if (!wmm) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < GWM_MAX_TIMERS; i++) if (wmm->timers[i].used) n++;
    return n;
}
uint32_t gwm_live_wave_out(const GuestWinmm *wmm) {
    return wmm ? wmm->live_wave_out : 0u;
}
uint32_t gwm_bytes_written(const GuestWinmm *wmm) {
    return wmm ? wmm->bytes_written : 0u;
}
uint32_t gwm_timer_fires(const GuestWinmm *wmm) {
    return wmm ? wmm->timer_fires : 0u;
}
uint32_t gwm_wave_out_done(const GuestWinmm *wmm) {
    return wmm ? wmm->wave_done_count : 0u;
}
uint32_t gwm_wave_out_callbacks(const GuestWinmm *wmm) {
    return wmm ? wmm->wave_callbacks : 0u;
}
uint32_t gwm_wave_out_events(const GuestWinmm *wmm) {
    return wmm ? wmm->wave_events : 0u;
}
uint32_t gwm_wave_out_pending(const GuestWinmm *wmm) {
    if (!wmm) return 0u;
    uint32_t n = 0;
    for (uint32_t i = 0; i < GWM_MAX_WAVE_OUT; i++)
        if (wmm->waves[i]) n += wmm->waves[i]->pending_count;
    return n;
}
uint32_t gwm_wave_out_position(const GuestWinmm *wmm) {
    return wmm ? wmm->wave_position : 0u;
}
const char *gwm_backend_name(const GuestWinmm *wmm) {
    return (wmm && wmm->backend && wmm->backend->name) ? wmm->backend->name : NULL;
}
const char *gwm_last_stop(const GuestWinmm *wmm) {
    return (wmm && wmm->last_stop[0]) ? wmm->last_stop : NULL;
}
const char *gwm_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_WMM_NO_GUEST_CALL: return "winmm-no-guest-call-hook";
    case GU_STOP_WMM_TIMER_FLAGS: return "winmm-timer-flags-unsupported";
    case GU_STOP_WMM_CALLBACK: return "winmm-callback-type-unsupported";
    case GU_STOP_WMM_FORMAT: return "winmm-wave-format-unsupported";
    case GU_STOP_WMM_BAD_ARGS: return "winmm-argument-outside-model";
    case GU_STOP_WMM_TIMER_LIMIT: return "winmm-timer-table-full";
    case GU_STOP_WMM_BACKEND: return "winmm-audio-backend-failed";
    case GU_STOP_WMM_EVENT_HOOK: return "winmm-callback-event-hook-missing";
    case GU_STOP_WMM_QUEUE_FULL: return "winmm-wave-block-queue-full";
    default: return "unknown-stop";
    }
}

/* --- multimedia timers -------------------------------------------------- */
typedef struct {
    GwmTimer *timer;
    GuestWinmm *wmm;
} GwmTimerArg;

static void *timer_thread(void *argument) {
    GwmTimerArg *arg = (GwmTimerArg *)argument;
    GwmTimer *timer = arg->timer;
    GuestWinmm *wmm = arg->wmm;
    free(arg);
    for (;;) {
        uint64_t deadline = monotonic_ms() + timer->delay;
        for (;;) {
            if (timer->stop) return NULL;
            uint64_t now = monotonic_ms();
            if (now >= deadline) break;
            uint64_t remaining = deadline - now;
            if (remaining > 5) remaining = 5;
            struct timespec pause;
            pause.tv_sec = (time_t)(remaining / 1000u);
            pause.tv_nsec = (long)((remaining % 1000u) * 1000000u);
            (void)nanosleep(&pause, NULL);
        }
        if (timer->stop) return NULL;
        if (!wmm->call) return NULL;
        uint32_t args[5];
        args[0] = timer->id;
        args[1] = 0;
        args[2] = timer->user;
        args[3] = 0;
        args[4] = 0;
        uint32_t result = 0;
        /* The host adapter owns the guest CPU; a failure ends the timer. */
        if (wmm->call(wmm->call_context, timer->guest_proc, args, 5, &result))
            return NULL;
        wmm->timer_fires++;
        if (!timer->periodic) return NULL;
    }
}
int gwm_time_begin_period(GuestWinmm *wmm, const uint32_t *args,
                          uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    uint32_t period = args[0];
    if (period < 1 || period > 16) {
        *result = GWM_TIMERR_NOCANDO;
        return GM_OK;
    }
    /* The host does not change timer resolution; the counter is bookkeeping. */
    if (wmm->period_count == UINT32_MAX) return GM_UNSUPPORTED;
    wmm->period_count++;
    *result = GWM_TIMERR_NOERROR;
    return GM_OK;
}
int gwm_time_end_period(GuestWinmm *wmm, const uint32_t *args,
                        uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    uint32_t period = args[0];
    if (period < 1 || period > 16) {
        *result = GWM_TIMERR_NOCANDO;
        return GM_OK;
    }
    /* Documented: an unmatched timeEndPeriod is harmless. */
    if (wmm->period_count) wmm->period_count--;
    *result = GWM_TIMERR_NOERROR;
    return GM_OK;
}
int gwm_time_get_time(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    (void)args;
    if (!wmm || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = (uint32_t)monotonic_ms();
    return GM_OK;
}
int gwm_time_set_event(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    uint32_t delay = args[0];
    uint32_t flags = args[4];
    if (flags & ~(GWM_TIME_PERIODIC | GWM_TIME_CALLBACK_EVENT_SET |
                  GWM_TIME_CALLBACK_EVENT_PULSE | GWM_TIME_KILL_SYNCHRONOUS))
        return stop(wmm, GU_STOP_WMM_TIMER_FLAGS,
                    "timeSetEvent: timer flags not modelled");
    if (flags & (GWM_TIME_CALLBACK_EVENT_SET | GWM_TIME_CALLBACK_EVENT_PULSE))
        return stop(wmm, GU_STOP_WMM_CALLBACK,
                    "timeSetEvent: only TIME_CALLBACK_FUNCTION is modelled");
    if (!args[2]) return stop(wmm, GU_STOP_WMM_BAD_ARGS,
                              "timeSetEvent: lpTimeProc must not be NULL");
    if (!wmm->call) return stop(wmm, GU_STOP_WMM_NO_GUEST_CALL,
                                "timeSetEvent: no guest-call hook installed");
    if (!delay) delay = 1;
    pthread_mutex_lock(&wmm->lock);
    GwmTimer *timer = NULL;
    for (uint32_t i = 0; i < GWM_MAX_TIMERS; i++)
        if (!wmm->timers[i].used) { timer = &wmm->timers[i]; break; }
    if (!timer) {
        pthread_mutex_unlock(&wmm->lock);
        return stop(wmm, GU_STOP_WMM_TIMER_LIMIT,
                    "timeSetEvent: timer table full");
    }
    memset(timer, 0, sizeof(*timer));
    timer->used = 1;
    timer->id = wmm->next_timer_id++;
    if (!wmm->next_timer_id) wmm->next_timer_id = 1;
    timer->delay = delay;
    timer->periodic = (flags & GWM_TIME_PERIODIC) ? 1u : 0u;
    timer->guest_proc = args[2];
    timer->user = args[3];
    GwmTimerArg *argument = (GwmTimerArg *)calloc(1, sizeof(*argument));
    if (!argument) {
        timer->used = 0;
        pthread_mutex_unlock(&wmm->lock);
        return GM_ALLOCATION;
    }
    argument->timer = timer;
    argument->wmm = wmm;
    if (pthread_create(&timer->thread, NULL, timer_thread, argument) != 0) {
        free(argument);
        timer->used = 0;
        pthread_mutex_unlock(&wmm->lock);
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "timeSetEvent: host timer thread could not be created");
    }
    timer->thread_live = 1;
    uint32_t id = timer->id;
    pthread_mutex_unlock(&wmm->lock);
    *result = id;
    return GM_OK;
}
int gwm_time_kill_event(GuestWinmm *wmm, const uint32_t *args,
                        uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    uint32_t id = args[0];
    pthread_mutex_lock(&wmm->lock);
    GwmTimer *timer = NULL;
    for (uint32_t i = 0; i < GWM_MAX_TIMERS; i++)
        if (wmm->timers[i].used && wmm->timers[i].id == id) {
            timer = &wmm->timers[i];
            break;
        }
    if (!timer) {
        pthread_mutex_unlock(&wmm->lock);
        *result = GWM_MMSYSERR_INVALPARAM;
        return GM_OK;
    }
    timer->stop = 1;
    pthread_t thread = timer->thread;
    int live = timer->thread_live;
    timer->thread_live = 0;
    timer->used = 0;
    pthread_mutex_unlock(&wmm->lock);
    if (live) pthread_join(thread, NULL);
    *result = GWM_TIMERR_NOERROR;
    return GM_OK;
}

/* --- waveOut ------------------------------------------------------------ */
static GwmWaveOut *wave_lookup(uint32_t handle) {
    GwmWaveOut *wave = (GwmWaveOut *)gh_lookup(handle, GH_KIND_OTHER, NULL);
    if (!wave || wave->magic != GWM_WAVE_MAGIC) return NULL;
    return wave;
}

/* Delivers one waveOut notification in the callback form the guest chose.
 * CALLBACK_FUNCTION runs the guest procedure through the host call hook,
 * CALLBACK_EVENT signals the guest event through the host event hook and
 * CALLBACK_NULL only updates the header. */
static void wave_deliver(GuestWinmm *wmm, GwmWaveOut *wave, uint32_t message,
                         uint32_t param1, uint32_t param2) {
    if (wave->callback_type == GWM_CALLBACK_FUNCTION) {
        if (!wmm->call) return;
        uint32_t args[5];
        args[0] = wave->guest_handle;
        args[1] = message;
        args[2] = wave->user;
        args[3] = param1;
        args[4] = param2;
        uint32_t result = 0;
        if (wmm->call(wmm->call_context, wave->callback, args, 5, &result) == 0)
            wmm->wave_callbacks++;
    } else if (wave->callback_type == GWM_CALLBACK_EVENT) {
        if (!wmm->event_set) return;
        if (wmm->event_set(wmm->event_context, wave->callback) == 0)
            wmm->wave_events++;
    }
}

/* Marks a queued WAVEHDR done. Runs on the audio thread for the asynchronous
 * path, so it only touches guest memory through gm_write (which is the same
 * contract the multimedia timer callbacks already follow). */
static void wave_complete_header(GuestWinmm *wmm, uint32_t header) {
    uint8_t raw[GWM_WAVEHDR_SIZE];
    if (gm_read(wmm->memory, header, raw, sizeof(raw))) return;
    uint32_t flags = get32(raw + 16);
    put32(raw + 16, (flags | GWM_WHDR_DONE) & ~GWM_WHDR_INQUEUE);
    (void)gm_write(wmm->memory, header, raw, sizeof(raw));
}

static GwmWaveOut *wave_by_backend_locked(GuestWinmm *wmm,
                                          uint32_t backend_handle) {
    for (uint32_t i = 0; i < GWM_MAX_WAVE_OUT; i++)
        if (wmm->waves[i] && wmm->waves[i]->backend_handle == backend_handle)
            return wmm->waves[i];
    return NULL;
}

static void wave_block_done(void *context, uint32_t backend_handle,
                            uint32_t block_id) {
    GuestWinmm *wmm = (GuestWinmm *)context;
    if (!wmm) return;
    uint32_t header = 0;
    pthread_mutex_lock(&wmm->lock);
    GwmWaveOut *wave = wave_by_backend_locked(wmm, backend_handle);
    if (wave) {
        for (uint32_t i = 0; i < wave->pending_count; i++) {
            if (wave->pending[i].block_id != block_id) continue;
            header = wave->pending[i].header;
            for (uint32_t j = i + 1; j < wave->pending_count; j++)
                wave->pending[j - 1] = wave->pending[j];
            wave->pending_count--;
            break;
        }
    }
    pthread_mutex_unlock(&wmm->lock);
    if (!wave || !header) return;
    wave_complete_header(wmm, header);
    wmm->wave_done_count++;
    wave_deliver(wmm, wave, GWM_WOM_DONE, header, 0);
}
int gwm_wave_out_open(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    if (!args[0]) return stop(wmm, GU_STOP_WMM_BAD_ARGS,
                              "waveOutOpen: phwo must not be NULL");
    uint32_t device = args[1];
    if (device != 0 && device != GWM_WAVE_MAPPER) {
        int rc = write_u32(wmm, args[0], 0);
        if (rc) return rc;
        *result = GWM_MMSYSERR_NODRIVER;
        return GM_OK;
    }
    uint32_t flags = args[5];
    uint32_t callback_type = flags & GWM_CALLBACK_TYPEMASK;
    if (callback_type == GWM_CALLBACK_WINDOW ||
        callback_type == GWM_CALLBACK_THREAD)
        return stop(wmm, GU_STOP_WMM_CALLBACK,
                    "waveOutOpen: window/thread callbacks are not modelled");
    if (callback_type != GWM_CALLBACK_NULL &&
        callback_type != GWM_CALLBACK_FUNCTION &&
        callback_type != GWM_CALLBACK_EVENT)
        return stop(wmm, GU_STOP_WMM_CALLBACK,
                    "waveOutOpen: unknown callback type");
    if (callback_type == GWM_CALLBACK_FUNCTION && !wmm->call)
        return stop(wmm, GU_STOP_WMM_NO_GUEST_CALL,
                    "waveOutOpen: CALLBACK_FUNCTION without a guest-call hook");
    if (callback_type == GWM_CALLBACK_EVENT && !wmm->event_set)
        return stop(wmm, GU_STOP_WMM_EVENT_HOOK,
                    "waveOutOpen: CALLBACK_EVENT without an event hook");
    uint8_t format[GWM_MAX_FORMAT_BYTES];
    memset(format, 0, sizeof(format));
    int rc = gm_read(wmm->memory, args[2], format, GWM_WAVEFORMATEX_SIZE);
    if (rc) return rc;
    /* WAVE_FORMAT_EXTENSIBLE carries its sub-format behind cbSize; pass the
     * whole record to the backend so it can read the GUID. */
    uint32_t format_size = GWM_WAVEFORMATEX_SIZE;
    uint32_t extra = (uint32_t)format[16] | ((uint32_t)format[17] << 8);
    if (extra) {
        if (extra > GWM_MAX_FORMAT_BYTES - GWM_WAVEFORMATEX_SIZE)
            return stop(wmm, GU_STOP_WMM_FORMAT,
                        "waveOutOpen: cbSize larger than the modelled record");
        rc = gm_read(wmm->memory, args[2] + GWM_WAVEFORMATEX_SIZE,
                     format + GWM_WAVEFORMATEX_SIZE, extra);
        if (rc) return rc;
        format_size += extra;
    }
    uint32_t tag = (uint32_t)format[0] | ((uint32_t)format[1] << 8);
    if (tag != GWM_WAVE_FORMAT_PCM && tag != GWM_WAVE_FORMAT_IEEE_FLOAT &&
        tag != GWM_WAVE_FORMAT_EXTENSIBLE)
        return stop(wmm, GU_STOP_WMM_FORMAT,
                    "waveOutOpen: only PCM/IEEE-float/extensible are modelled");
    if (!wmm->backend) {
        rc = write_u32(wmm, args[0], 0);
        if (rc) return rc;
        *result = GWM_MMSYSERR_NODRIVER;
        return GM_OK;
    }
    uint32_t backend_handle = 0;
    rc = wmm->backend->open(wmm->backend_context, device, format,
                            format_size, &backend_handle);
    if (rc) return stop(wmm, GU_STOP_WMM_BACKEND,
                        "waveOutOpen: audio backend refused the format");
    GwmWaveOut *wave = (GwmWaveOut *)calloc(1, sizeof(*wave));
    if (!wave) return GM_ALLOCATION;
    wave->backend_handle = backend_handle;
    wave->device = device;
    memcpy(wave->format, format, GWM_WAVEFORMATEX_SIZE);
    wave->magic = GWM_WAVE_MAGIC;
    wave->callback_type = callback_type;
    wave->callback = args[3];
    wave->user = args[4];
    uint32_t handle = gh_insert(GH_KIND_OTHER, wave, wave_destroy);
    if (!handle) { free(wave); return GM_ALLOCATION; }
    wave->guest_handle = handle;
    pthread_mutex_lock(&wmm->lock);
    GwmWaveOut **slot = NULL;
    for (uint32_t i = 0; i < GWM_MAX_WAVE_OUT; i++)
        if (!wmm->waves[i]) { slot = &wmm->waves[i]; break; }
    if (slot) *slot = wave;
    pthread_mutex_unlock(&wmm->lock);
    if (!slot) {
        (void)gh_close(handle);
        return stop(wmm, GU_STOP_WMM_QUEUE_FULL,
                    "waveOutOpen: waveOut device table full");
    }
    wmm->live_wave_out++;
    rc = write_u32(wmm, args[0], handle);
    if (rc) return rc;
    wave_deliver(wmm, wave, GWM_WOM_OPEN, 0, 0);
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_close(GuestWinmm *wmm, const uint32_t *args,
                       uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    /* Documented: closing a device with queued buffers fails. */
    if (wave->pending_count) {
        *result = GWM_WAVERR_STILLPLAYING;
        return GM_OK;
    }
    if (wmm->backend && wmm->backend->close)
        (void)wmm->backend->close(wmm->backend_context, wave->backend_handle);
    pthread_mutex_lock(&wmm->lock);
    for (uint32_t i = 0; i < GWM_MAX_WAVE_OUT; i++)
        if (wmm->waves[i] == wave) wmm->waves[i] = NULL;
    pthread_mutex_unlock(&wmm->lock);
    wave_deliver(wmm, wave, GWM_WOM_CLOSE, 0, 0);
    if (!gh_close(args[0])) return stop(wmm, GU_STOP_WMM_BACKEND,
                                        "waveOutClose: handle table refused");
    if (wmm->live_wave_out) wmm->live_wave_out--;
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_prepare_header(GuestWinmm *wmm, const uint32_t *args,
                                uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    if (!wave_lookup(args[0])) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    uint8_t header[GWM_WAVEHDR_SIZE];
    int rc = gm_read(wmm->memory, args[1], header, sizeof(header));
    if (rc) return rc;
    if (get32(header) == 0) {
        *result = GWM_MMSYSERR_INVALPARAM;
        return GM_OK;
    }
    uint32_t flags = get32(header + 16) | GWM_WHDR_PREPARED;
    put32(header + 16, flags);
    rc = gm_write(wmm->memory, args[1], header, sizeof(header));
    if (rc) return rc;
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_message(GuestWinmm *wmm, const uint32_t *args,
                         uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    /* No driver message interface exists in this runtime. */
    fprintf(stderr, "winmm: waveOutMessage(%#x) -> MMSYSERR_NOTSUPPORTED\n",
            args[1]);
    *result = GWM_MMSYSERR_NOTSUPPORTED;
    return GM_OK;
}

/* waveOutWrite is the real submission path: the prepared WAVEHDR data goes to
 * the backend, and the header is marked WHDR_DONE like a completed buffer. */
int gwm_wave_out_write(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    if (!wmm->backend || !wmm->backend->write)
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutWrite: no audio backend installed");
    uint8_t header[GWM_WAVEHDR_SIZE];
    int rc = gm_read(wmm->memory, args[1], header, sizeof(header));
    if (rc) return rc;
    uint32_t data = get32(header + 0);
    uint32_t length = get32(header + 4);
    uint32_t flags = get32(header + 16);
    if (!(flags & GWM_WHDR_PREPARED))
        return stop(wmm, GU_STOP_WMM_BAD_ARGS,
                    "waveOutWrite: header was not prepared");
    if (length > 1024u * 1024u)
        return stop(wmm, GU_STOP_WMM_BAD_ARGS,
                    "waveOutWrite: buffer larger than 1 MiB");
    if (length && !data)
        return stop(wmm, GU_STOP_WMM_BAD_ARGS,
                    "waveOutWrite: NULL lpData with a length");
    if (length) {
        rc = gm_check(wmm->memory, data, length, GM_READ);
        if (rc) return rc;
    }
    uint32_t async = (wmm->backend->enqueue && wmm->backend->set_done) ? 1u : 0u;
    if (!async) {
        /* Synchronous backend (a16 null backend, host capture): the whole
         * buffer is consumed inside this call and the header is done. */
        if (length) {
            uint8_t chunk[4096];
            uint32_t done = 0;
            while (done < length) {
                uint32_t step = length - done;
                if (step > sizeof(chunk)) step = sizeof(chunk);
                rc = gm_read(wmm->memory, data + done, chunk, step);
                if (rc) return rc;
                rc = wmm->backend->write(wmm->backend_context,
                                         wave->backend_handle, chunk, step);
                if (rc) return stop(wmm, GU_STOP_WMM_BACKEND,
                                    "waveOutWrite: backend write failed");
                done += step;
            }
            wmm->bytes_written += length;
        }
        put32(header + 16, (flags | GWM_WHDR_DONE) & ~GWM_WHDR_INQUEUE);
        rc = gm_write(wmm->memory, args[1], header, sizeof(header));
        if (rc) return rc;
        *result = GWM_MMSYSERR_NOERROR;
        return GM_OK;
    }
    /* Asynchronous path: copy the block once, queue it, and let the backend
     * report completion. WHDR_INQUEUE is visible before enqueue so that a
     * backend completing synchronously still sees a consistent header. */
    uint8_t *buffer = NULL;
    if (length) {
        buffer = (uint8_t *)malloc(length);
        if (!buffer) return GM_ALLOCATION;
        uint32_t done = 0;
        while (done < length) {
            uint32_t step = length - done;
            if (step > 4096u) step = 4096u;
            rc = gm_read(wmm->memory, data + done, buffer + done, step);
            if (rc) { free(buffer); return rc; }
            done += step;
        }
    }
    put32(header + 16, (flags | GWM_WHDR_INQUEUE) & ~GWM_WHDR_DONE);
    rc = gm_write(wmm->memory, args[1], header, sizeof(header));
    if (rc) { free(buffer); return rc; }
    pthread_mutex_lock(&wmm->lock);
    if (wave->pending_count >= GWM_MAX_PENDING_BLOCKS) {
        pthread_mutex_unlock(&wmm->lock);
        free(buffer);
        return stop(wmm, GU_STOP_WMM_QUEUE_FULL,
                    "waveOutWrite: per-device block queue full");
    }
    uint32_t block_id = ++wmm->next_block_id;
    if (!block_id) block_id = ++wmm->next_block_id;
    wave->pending[wave->pending_count].header = args[1];
    wave->pending[wave->pending_count].block_id = block_id;
    wave->pending_count++;
    pthread_mutex_unlock(&wmm->lock);
    rc = wmm->backend->enqueue(wmm->backend_context, wave->backend_handle,
                               block_id, buffer, length);
    free(buffer);
    if (rc) {
        pthread_mutex_lock(&wmm->lock);
        for (uint32_t i = 0; i < wave->pending_count; i++) {
            if (wave->pending[i].block_id != block_id) continue;
            for (uint32_t j = i + 1; j < wave->pending_count; j++)
                wave->pending[j - 1] = wave->pending[j];
            wave->pending_count--;
            break;
        }
        pthread_mutex_unlock(&wmm->lock);
        /* Restore the header: nothing will ever complete this block, so it
         * must not stay WHDR_INQUEUE (that would look like a running buffer). */
        put32(header + 16, flags & ~(GWM_WHDR_INQUEUE | GWM_WHDR_DONE));
        (void)gm_write(wmm->memory, args[1], header, sizeof(header));
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutWrite: audio backend refused the block");
    }
    wmm->bytes_written += length;
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_reset(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    if (wmm->backend && wmm->backend->reset) {
        int rc = wmm->backend->reset(wmm->backend_context, wave->backend_handle);
        if (rc) return stop(wmm, GU_STOP_WMM_BACKEND,
                            "waveOutReset: backend reset failed");
    }
    /* Documented: reset marks every queued buffer done and reports WOM_DONE. */
    uint32_t headers[GWM_MAX_PENDING_BLOCKS];
    uint32_t count = 0;
    pthread_mutex_lock(&wmm->lock);
    for (uint32_t i = 0; i < wave->pending_count; i++)
        headers[count++] = wave->pending[i].header;
    wave->pending_count = 0;
    pthread_mutex_unlock(&wmm->lock);
    for (uint32_t i = 0; i < count; i++) {
        wave_complete_header(wmm, headers[i]);
        wmm->wave_done_count++;
        wave_deliver(wmm, wave, GWM_WOM_DONE, headers[i], 0);
    }
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_pause(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    if (!wmm->backend || !wmm->backend->pause)
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutPause: audio backend cannot pause");
    if (wmm->backend->pause(wmm->backend_context, wave->backend_handle, 1))
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutPause: backend pause failed");
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_restart(GuestWinmm *wmm, const uint32_t *args,
                         uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    if (!wmm->backend || !wmm->backend->pause)
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutRestart: audio backend cannot resume");
    if (wmm->backend->pause(wmm->backend_context, wave->backend_handle, 0))
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutRestart: backend resume failed");
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
/* waveOutGetPosition(HWAVEOUT, LPMMTIME, UINT). MMTime is wType + a DWORD
 * union (8 bytes on PE32); TIME_MS/TIME_SAMPLES/TIME_BYTES are derived from
 * the backend's rendered byte count and the open format. */
int gwm_wave_out_get_position(GuestWinmm *wmm, const uint32_t *args,
                              uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    if (!args[1] || args[2] < GWM_MMTIME_SIZE) {
        *result = GWM_MMSYSERR_INVALPARAM;
        return GM_OK;
    }
    uint8_t raw[GWM_MMTIME_SIZE];
    int rc = gm_read(wmm->memory, args[1], raw, sizeof(raw));
    if (rc) return rc;
    uint32_t type = get32(raw);
    uint32_t bytes = 0;
    if (wmm->backend && wmm->backend->position)
        bytes = wmm->backend->position(wmm->backend_context,
                                       wave->backend_handle);
    else
        bytes = wmm->bytes_written;
    wmm->wave_position = bytes;
    uint32_t rate = get32(wave->format + 8);  /* nAvgBytesPerSec */
    uint32_t align = (uint32_t)wave->format[12] |
                     ((uint32_t)wave->format[13] << 8);
    uint32_t value = 0;
    switch (type) {
    case GWM_TIME_MS:
        value = rate ? (uint32_t)(((uint64_t)bytes * 1000u) / rate) : 0u;
        break;
    case GWM_TIME_SAMPLES:
        value = align ? bytes / align : 0u;
        break;
    case GWM_TIME_BYTES:
        value = bytes;
        break;
    default:
        return stop(wmm, GU_STOP_WMM_FORMAT,
                    "waveOutGetPosition: unsupported MMTime type");
    }
    put32(raw, type);
    put32(raw + 4, value);
    rc = gm_write(wmm->memory, args[1], raw, sizeof(raw));
    if (rc) return rc;
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_unprepare_header(GuestWinmm *wmm, const uint32_t *args,
                                  uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    if (!wave_lookup(args[0])) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    uint8_t header[GWM_WAVEHDR_SIZE];
    int rc = gm_read(wmm->memory, args[1], header, sizeof(header));
    if (rc) return rc;
    uint32_t flags = get32(header + 16);
    if (flags & GWM_WHDR_INQUEUE) {
        *result = GWM_WAVERR_STILLPLAYING;
        return GM_OK;
    }
    put32(header + 16, flags & ~GWM_WHDR_PREPARED);
    rc = gm_write(wmm->memory, args[1], header, sizeof(header));
    if (rc) return rc;
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_get_num_devs(GuestWinmm *wmm, const uint32_t *args,
                              uint32_t *result) {
    (void)args;
    if (!wmm || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = wmm->backend ? 1u : 0u;
    return GM_OK;
}
static int wave_out_caps(GuestWinmm *wmm, const uint32_t *args,
                         uint32_t *result, uint32_t wide) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    uint32_t needed = wide ? GWM_WAVEOUTCAPS_W_SIZE : GWM_WAVEOUTCAPS_A_SIZE;
    if (!args[1] || args[2] < needed) {
        *result = GWM_MMSYSERR_INVALPARAM;
        return GM_OK;
    }
    uint8_t caps[GWM_WAVEOUTCAPS_W_SIZE];
    memset(caps, 0, sizeof(caps));
    caps[0] = 1;    /* wMid: the port's own driver id */
    caps[2] = 1;    /* wPid */
    put32(caps + 4, 0x00010000u); /* vDriverVersion 1.0 */
    static const char name[] = "BurnoutPR port waveOut";
    uint32_t unit = wide ? 2u : 1u;
    uint32_t limit = wide ? 31u : 31u;
    for (uint32_t i = 0; i < limit && name[i]; i++) {
        caps[8 + i * unit] = (uint8_t)name[i];
        if (wide) caps[8 + i * unit + 1] = 0;
    }
    uint32_t formats_offset = 8 + 32u * unit;
    put32(caps + formats_offset, 0x00000004u); /* WAVE_FORMAT_2M08 (PCM) */
    caps[formats_offset + 4] = 2;              /* wChannels */
    put32(caps + formats_offset + 8, 0);       /* dwSupport: no volume/mixer */
    int rc = gm_write(wmm->memory, args[1], caps, needed);
    if (rc) return rc;
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}
int gwm_wave_out_get_dev_caps_a(GuestWinmm *wmm, const uint32_t *args,
                                uint32_t *result) {
    return wave_out_caps(wmm, args, result, 0);
}
int gwm_wave_out_get_dev_caps_w(GuestWinmm *wmm, const uint32_t *args,
                                uint32_t *result) {
    return wave_out_caps(wmm, args, result, 1);
}
int gwm_wave_out_set_volume(GuestWinmm *wmm, const uint32_t *args,
                            uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    GwmWaveOut *wave = wave_lookup(args[0]);
    if (!wave) { *result = GWM_MMSYSERR_INVALHANDLE; return GM_OK; }
    if (!wmm->backend || !wmm->backend->set_volume) {
        /* No mixer control exists: the documented "not supported" answer. */
        fprintf(stderr, "winmm: waveOutSetVolume(%#x) -> MMSYSERR_NOTSUPPORTED\n",
                args[1]);
        *result = GWM_MMSYSERR_NOTSUPPORTED;
        return GM_OK;
    }
    uint32_t volume = args[1];
    if (wmm->backend->set_volume(wmm->backend_context, wave->backend_handle,
                                 volume & 0xffffu, volume >> 16))
        return stop(wmm, GU_STOP_WMM_BACKEND,
                    "waveOutSetVolume: backend refused the volume");
    *result = GWM_MMSYSERR_NOERROR;
    return GM_OK;
}

/* --- waveIn (no capture device) ----------------------------------------- */
int gwm_wave_in_open(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    if (!args[0]) return stop(wmm, GU_STOP_WMM_BAD_ARGS,
                              "waveInOpen: phwi must not be NULL");
    int rc = write_u32(wmm, args[0], 0);
    if (rc) return rc;
    /* Documented: no capture device is present. */
    *result = GWM_MMSYSERR_NODRIVER;
    return GM_OK;
}
int gwm_wave_in_close(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = GWM_MMSYSERR_NODRIVER;
    return GM_OK;
}
int gwm_wave_in_prepare_header(GuestWinmm *wmm, const uint32_t *args,
                               uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = GWM_MMSYSERR_NODRIVER;
    return GM_OK;
}
int gwm_wave_in_message(GuestWinmm *wmm, const uint32_t *args,
                        uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = GWM_MMSYSERR_NODRIVER;
    return GM_OK;
}
int gwm_wave_in_add_buffer(GuestWinmm *wmm, const uint32_t *args,
                           uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = GWM_MMSYSERR_NODRIVER;
    return GM_OK;
}
int gwm_wave_in_start(GuestWinmm *wmm, const uint32_t *args, uint32_t *result) {
    if (!wmm || !args || !result) return GU_STOP_WMM_BAD_ARGS;
    *result = GWM_MMSYSERR_NODRIVER;
    return GM_OK;
}

/* --- registration list -------------------------------------------------- */
#define GWM_ARGS(n) ((n) * 4u)
const GuestWinmmExport winmm_exports[] = {
    { "winmm.dll", "timeBeginPeriod", GWM_ARGS(1), gwm_time_begin_period },
    { "winmm.dll", "timeEndPeriod", GWM_ARGS(1), gwm_time_end_period },
    { "winmm.dll", "timeGetTime", GWM_ARGS(0), gwm_time_get_time },
    { "winmm.dll", "timeSetEvent", GWM_ARGS(5), gwm_time_set_event },
    { "winmm.dll", "timeKillEvent", GWM_ARGS(1), gwm_time_kill_event },
    { "winmm.dll", "waveOutOpen", GWM_ARGS(6), gwm_wave_out_open },
    { "winmm.dll", "waveOutClose", GWM_ARGS(1), gwm_wave_out_close },
    { "winmm.dll", "waveOutPrepareHeader", GWM_ARGS(3),
      gwm_wave_out_prepare_header },
    { "winmm.dll", "waveOutMessage", GWM_ARGS(4), gwm_wave_out_message },
    { "winmm.dll", "waveInOpen", GWM_ARGS(6), gwm_wave_in_open },
    { "winmm.dll", "waveInClose", GWM_ARGS(1), gwm_wave_in_close },
    { "winmm.dll", "waveInPrepareHeader", GWM_ARGS(3),
      gwm_wave_in_prepare_header },
    { "winmm.dll", "waveInMessage", GWM_ARGS(4), gwm_wave_in_message },
    /* resolved dynamically by the game through GetProcAddress */
    { "winmm.dll", "waveOutWrite", GWM_ARGS(3), gwm_wave_out_write },
    { "winmm.dll", "waveOutReset", GWM_ARGS(1), gwm_wave_out_reset },
    { "winmm.dll", "waveOutUnprepareHeader", GWM_ARGS(3),
      gwm_wave_out_unprepare_header },
    { "winmm.dll", "waveOutGetNumDevs", GWM_ARGS(0),
      gwm_wave_out_get_num_devs },
    { "winmm.dll", "waveOutGetDevCapsA", GWM_ARGS(3),
      gwm_wave_out_get_dev_caps_a },
    { "winmm.dll", "waveOutGetDevCapsW", GWM_ARGS(3),
      gwm_wave_out_get_dev_caps_w },
    { "winmm.dll", "waveOutSetVolume", GWM_ARGS(2), gwm_wave_out_set_volume },
    { "winmm.dll", "waveOutPause", GWM_ARGS(1), gwm_wave_out_pause },
    { "winmm.dll", "waveOutRestart", GWM_ARGS(1), gwm_wave_out_restart },
    { "winmm.dll", "waveOutGetPosition", GWM_ARGS(3),
      gwm_wave_out_get_position },
    { "winmm.dll", "waveInAddBuffer", GWM_ARGS(3), gwm_wave_in_add_buffer },
    { "winmm.dll", "waveInStart", GWM_ARGS(1), gwm_wave_in_start }
};
const uint32_t winmm_export_count =
    (uint32_t)(sizeof(winmm_exports) / sizeof(winmm_exports[0]));
