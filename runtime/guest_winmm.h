#ifndef BURNOUT_GUEST_WINMM_H
#define BURNOUT_GUEST_WINMM_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"

/* winmm bridges: multimedia timers and the waveOut audio path.
 *
 * The static import census shows the game uses winmm waveOut (waveOutOpen,
 * waveOutPrepareHeader, waveOutClose, waveOutMessage) and four waveIn entry
 * points; there is no XAudio2, DirectSound CLSID or WASAPI usage in the image
 * ((development evidence, not published)).
 *
 * Modelled:
 *   - timeBeginPeriod/timeEndPeriod: a period reference counter. The host does
 *     not change timer resolution, so the counter is bookkeeping only; the
 *     documented TIMERR_NOERROR is returned for 1..16.
 *   - timeGetTime: CLOCK_MONOTONIC milliseconds, 32-bit wrapping.
 *   - timeSetEvent/timeKillEvent: a real pthread per timer. The callback runs in
 *     the guest through the host-installed hook gwm_set_guest_call(); without a
 *     hook the call is a named stop instead of a silent no-op. Only
 *     TIME_CALLBACK_FUNCTION is modelled (no event/window/thread callbacks).
 *   - waveOutOpen/Close/PrepareHeader/Message: real handle objects with a
 *     WAVEFORMATEX copy, driven through the gwm_audio_backend interface. The
 *     Android build installs an AAudio backend; host tests install the null
 *     backend. Without a backend every open returns the documented
 *     MMSYSERR_NODRIVER.
 *   - waveInOpen/Close/PrepareHeader/Message: MMSYSERR_NODRIVER (6). No capture
 *     device exists in this runtime and none is invented.
 *
 * Documented limits: no waveOutWrite/Reset/UnprepareHeader (not imported, and
 * no mixing graph exists), no CALLBACK_WINDOW/THREAD/EVENT, no device
 * enumeration, no volume control, no sample-rate conversion.
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. A non-zero
 * function return is a named stop, never a Win32 status. */

#define GWM_MMSYSERR_NOERROR 0u
#define GWM_MMSYSERR_ERROR 1u
#define GWM_MMSYSERR_INVALHANDLE 5u
#define GWM_MMSYSERR_NODRIVER 6u
#define GWM_MMSYSERR_NOMEM 7u
#define GWM_MMSYSERR_NOTSUPPORTED 8u
#define GWM_MMSYSERR_INVALPARAM 11u
#define GWM_TIMERR_NOERROR 0u
#define GWM_TIMERR_NOCANDO 97u
#define GWM_WAVERR_STILLPLAYING 33u
#define GWM_WAVEOUTCAPS_A_SIZE 52u
#define GWM_WAVEOUTCAPS_W_SIZE 84u

#define GWM_WAVE_MAPPER 0xffffffffu
#define GWM_WAVE_FORMAT_PCM 1u
#define GWM_WAVE_FORMAT_IEEE_FLOAT 3u
#define GWM_WAVE_FORMAT_EXTENSIBLE 0xfffeu
#define GWM_WAVEFORMATEX_SIZE 18u
#define GWM_WAVEHDR_SIZE 32u
#define GWM_WHDR_DONE 0x00000001u
#define GWM_WHDR_PREPARED 0x00000002u
#define GWM_WHDR_INQUEUE 0x00000010u

#define GWM_WOM_OPEN 0x3bbu
#define GWM_WOM_CLOSE 0x3bcu
#define GWM_WOM_DONE 0x3bdu
#define GWM_MMTIME_SIZE 8u
#define GWM_TIME_MS 0x0001u
#define GWM_TIME_SAMPLES 0x0002u
#define GWM_TIME_BYTES 0x0004u
#define GWM_MAX_WAVE_OUT 8u
#define GWM_MAX_PENDING_BLOCKS 64u
#define GWM_MAX_FORMAT_BYTES 40u

#define GWM_CALLBACK_NULL 0x00000000u
#define GWM_CALLBACK_WINDOW 0x00010000u
#define GWM_CALLBACK_THREAD 0x00020000u
#define GWM_CALLBACK_FUNCTION 0x00030000u
#define GWM_CALLBACK_EVENT 0x00050000u
#define GWM_CALLBACK_TYPEMASK 0x00070000u

#define GWM_TIME_ONESHOT 0x0000u
#define GWM_TIME_PERIODIC 0x0001u
#define GWM_TIME_CALLBACK_FUNCTION 0x0000u
#define GWM_TIME_CALLBACK_EVENT_SET 0x0010u
#define GWM_TIME_CALLBACK_EVENT_PULSE 0x0020u
#define GWM_TIME_KILL_SYNCHRONOUS 0x0100u
#define GWM_MAX_TIMERS 32u

/* Module-local named stops. */
enum {
    GU_STOP_WMM_NO_GUEST_CALL = -2601,
    GU_STOP_WMM_TIMER_FLAGS = -2602,
    GU_STOP_WMM_CALLBACK = -2603,
    GU_STOP_WMM_FORMAT = -2604,
    GU_STOP_WMM_BAD_ARGS = -2605,
    GU_STOP_WMM_TIMER_LIMIT = -2606,
    GU_STOP_WMM_BACKEND = -2607,
    GU_STOP_WMM_EVENT_HOOK = -2608,
    GU_STOP_WMM_QUEUE_FULL = -2609
};

/* Completion notification for the asynchronous audio path (A18).
 * The backend calls `done` from its own audio/delivery thread once the block it
 * accepted through `enqueue` has been rendered. `block_id` is echoed back
 * unchanged. The winmm module then marks the WAVEHDR WHDR_DONE and delivers
 * WOM_DONE in the callback form the guest chose. */
typedef void (*GwmAudioBlockDone)(void *context, uint32_t backend_handle,
                                  uint32_t block_id);
/* CALLBACK_EVENT delivery. The host installs a hook that forwards to
 * guest_sync_objects gso_set_event() (the winmm module stays free of that
 * dependency). Returns 0 when the event was signalled. */
typedef int (*GwmEventSet)(void *context, uint32_t handle);

/* Host audio backend. All entry points return 0 on success and a non-zero
 * model stop on failure; the guest never sees a host pointer. */
typedef struct {
    const char *name;
    int (*open)(void *context, uint32_t device, const uint8_t *format,
                uint32_t format_size, uint32_t *backend_handle);
    int (*write)(void *context, uint32_t backend_handle, const uint8_t *data,
                 uint32_t size);
    int (*reset)(void *context, uint32_t backend_handle);
    int (*close)(void *context, uint32_t backend_handle);
    /* --- A18 additions, all optional (zero = not supported) -------------
     * A backend that fills in `enqueue` AND `set_done` gets real
     * asynchronous waveOut semantics: one call per WAVEHDR, the header stays
     * WHDR_INQUEUE until the backend reports the block done, and WOM_DONE is
     * delivered through the guest's callback. A backend without them (the
     * a16 null backend, the host capture backend) keeps the synchronous
     * "write everything, mark done" path. */
    int (*enqueue)(void *context, uint32_t backend_handle, uint32_t block_id,
                   const uint8_t *data, uint32_t size);
    void (*set_done)(void *context, GwmAudioBlockDone done, void *done_context);
    int (*pause)(void *context, uint32_t backend_handle, uint32_t paused);
    /* Bytes rendered since open (monotonic, 32-bit wrapping). */
    uint32_t (*position)(void *context, uint32_t backend_handle);
    int (*set_volume)(void *context, uint32_t backend_handle, uint32_t left,
                      uint32_t right);
    uint32_t (*underruns)(void *context, uint32_t backend_handle);
    uint32_t (*rebuilds)(void *context, uint32_t backend_handle);
} GwmAudioBackend;

/* Guest stdcall call hook, same shape as GuGuestCall. */
typedef int (*GwmGuestCall)(void *context, uint32_t address,
                            const uint32_t *args, uint32_t count,
                            uint32_t *result);

typedef struct GuestWinmm GuestWinmm;

int gwm_create(GuestMemory *, uint32_t teb, GuestWinmm **out);
void gwm_destroy(GuestWinmm *);
/* Installs the audio backend (borrowed). NULL removes it. */
int gwm_set_audio_backend(GuestWinmm *, const GwmAudioBackend *, void *context);
void gwm_set_guest_call(GuestWinmm *, GwmGuestCall, void *context);
/* CALLBACK_EVENT delivery hook (guest_sync_objects gso_set_event). */
void gwm_set_event_hook(GuestWinmm *, GwmEventSet, void *context);
/* The host-test backend: accepts every format and only counts bytes. */
const GwmAudioBackend *gwm_null_backend(void);
uint32_t gwm_null_bytes(void);

/* Diagnostics. */
uint32_t gwm_period_count(const GuestWinmm *);
uint32_t gwm_live_timers(const GuestWinmm *);
uint32_t gwm_live_wave_out(const GuestWinmm *);
uint32_t gwm_bytes_written(const GuestWinmm *);
uint32_t gwm_timer_fires(const GuestWinmm *);
/* A18 waveOut completion accounting. */
uint32_t gwm_wave_out_done(const GuestWinmm *);
uint32_t gwm_wave_out_callbacks(const GuestWinmm *);
uint32_t gwm_wave_out_events(const GuestWinmm *);
uint32_t gwm_wave_out_pending(const GuestWinmm *);
uint32_t gwm_wave_out_position(const GuestWinmm *);
const char *gwm_backend_name(const GuestWinmm *);
const char *gwm_last_stop(const GuestWinmm *);
const char *gwm_stop_name(int code);

int gwm_time_begin_period(GuestWinmm *, const uint32_t *args,
                          uint32_t *result);
int gwm_time_end_period(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_time_get_time(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_time_set_event(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_time_kill_event(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_open(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_close(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_prepare_header(GuestWinmm *, const uint32_t *args,
                                uint32_t *result);
int gwm_wave_out_message(GuestWinmm *, const uint32_t *args, uint32_t *result);
/* The rest of the waveOut family is resolved by the game through
 * GetProcAddress (the runtime name pool in the image contains waveOutWrite,
 * waveOutReset, waveOutUnprepareHeader, waveOutGetNumDevs, waveOutGetDevCaps and
 * waveOutSetVolume), so these are registered as well. */
int gwm_wave_out_write(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_reset(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_unprepare_header(GuestWinmm *, const uint32_t *args,
                                  uint32_t *result);
int gwm_wave_out_get_num_devs(GuestWinmm *, const uint32_t *args,
                              uint32_t *result);
int gwm_wave_out_get_dev_caps_a(GuestWinmm *, const uint32_t *args,
                                uint32_t *result);
int gwm_wave_out_get_dev_caps_w(GuestWinmm *, const uint32_t *args,
                                uint32_t *result);
int gwm_wave_out_set_volume(GuestWinmm *, const uint32_t *args,
                            uint32_t *result);
int gwm_wave_out_pause(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_restart(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_out_get_position(GuestWinmm *, const uint32_t *args,
                              uint32_t *result);
int gwm_wave_in_add_buffer(GuestWinmm *, const uint32_t *args,
                           uint32_t *result);
int gwm_wave_in_start(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_in_open(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_in_close(GuestWinmm *, const uint32_t *args, uint32_t *result);
int gwm_wave_in_prepare_header(GuestWinmm *, const uint32_t *args,
                               uint32_t *result);
int gwm_wave_in_message(GuestWinmm *, const uint32_t *args, uint32_t *result);

typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestWinmm *, const uint32_t *args, uint32_t *result);
} GuestWinmmExport;

extern const GuestWinmmExport winmm_exports[];
extern const uint32_t winmm_export_count;

#endif
