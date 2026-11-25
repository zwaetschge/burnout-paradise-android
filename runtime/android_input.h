#ifndef BURNOUT_ANDROID_INPUT_H
#define BURNOUT_ANDROID_INPUT_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_dinput.h"
#include "guest_user32.h"
#include "../android/jni/port_host_api.h"

/* Android input layer: Android key/touch/gamepad events -> the two guest input
 * models the image actually reads.
 *
 *  1. DirectInput host state (runtime/guest_dinput.h gdi_set_host_state):
 *     256 DIK scancodes for the keyboard, plus a DIJOYSTATE2-compatible
 *     joystick state (6 axes, 4 POV hats, 32 button bytes). The GUID census
 *     ((development evidence, not published)) shows the game only uses
 *     GUID_SysKeyboard, so the keyboard path is the important one.
 *  2. user32 messages (runtime/guest_user32.h gu_post_host_input):
 *     WM_KEYDOWN/WM_KEYUP with Win32 virtual-key codes, WM_ACTIVATE on focus
 *     changes, WM_SIZE on surface resize and WM_CLOSE on the back gesture.
 *
 * Because the game reads the keyboard, a gamepad is mapped onto virtual keys
 * (default: a racing layout - left stick/DPAD steer, RT/A accelerate, LT/X
 * brake, ...). The mapping is a table and can be overridden per key with
 * <user_root>/gamepad-keys.ini:
 *
 *     accelerate = UP
 *     brake      = DOWN
 *     steer_left = LEFT
 *     handbrake  = SPACE
 *     boost      = none
 *
 * Touch is a zone model only (no rendering): the lower half of the surface is
 * split into steering (left/right) and pedals (brake/gas); the upper half is a
 * menu tap zone (VK_RETURN). Zones produce the same virtual keys as the pad.
 *
 * Every VK is reference counted per source (front-end key event, gamepad,
 * touch), so releasing one source never clears a key another source still
 * holds. Guest memory is only touched through the guest_dinput/guest_user32
 * modules; this file never writes guest memory itself. */

#define AIN_MAX_POINTERS 8u
#define AIN_MAX_BINDINGS 32u
#define AIN_VK_COUNT 256u
#define AIN_PATH_MAX 512u

/* Input sources; also the holder bits per virtual key. */
enum {
    AIN_SRC_KEYBOARD = 0, /* front-end key events (ain_key_event) */
    AIN_SRC_GAMEPAD = 1,
    AIN_SRC_TOUCH = 2,
    AIN_SRC_COUNT = 3
};

/* Touch zones. */
enum {
    AIN_ZONE_NONE = 0,
    AIN_ZONE_STEER_LEFT = 1,
    AIN_ZONE_STEER_RIGHT = 2,
    AIN_ZONE_BRAKE = 3,
    AIN_ZONE_GAS = 4,
    AIN_ZONE_MENU = 5,
    AIN_ZONE_COUNT = 6
};

/* Binding sources for the gamepad -> keyboard table. */
enum {
    AIN_SRC_NONE = 0,
    AIN_SRC_BUTTON,   /* index = PORT_PAD_* bit */
    AIN_SRC_DPAD,     /* index = PORT_PAD_DPAD_* bit */
    AIN_SRC_AXIS_POS, /* index = PORT_AXIS_*, active above +threshold */
    AIN_SRC_AXIS_NEG  /* index = PORT_AXIS_*, active below -threshold */
};

typedef struct {
    const char *name;  /* ini key name */
    uint32_t source;   /* AIN_SRC_* */
    uint32_t index;    /* button/dpad bit or axis index */
    int32_t threshold; /* axis threshold in 1/1000 units */
    uint32_t vk;       /* 0 = disabled */
} AinBinding;

typedef struct AndroidInput AndroidInput;

/* Routes the gamepad to an image that reads XInput itself (see the .c file). */
void ain_set_gamepad_sink(int (*sink)(const port_gamepad_state *state));
/* 1 = the image reads the pad itself (see android_input.c). */
int ain_default_set_native_gamepad(int enabled);
int ain_default_set_targets(GuestDinput *dinput, GuestUser32 *user32);

AndroidInput *ain_create(void);
void ain_destroy(AndroidInput *);
/* The two guest models this layer feeds. Both are borrowed; NULL detaches. */
int ain_set_targets(AndroidInput *, GuestDinput *, GuestUser32 *);
/* Loads <user_root>/gamepad-keys.ini when it exists (missing file keeps the
 * built-in racing layout). */
int ain_set_user_root(AndroidInput *, const char *user_root);

/* --- events from the front end ----------------------------------------- */
int ain_key_event(AndroidInput *, int32_t akeycode, uint32_t down);
/* action: 0 = down, 1 = up, 2 = move, 3 = cancel, 5 = pointer down,
 * 6 = pointer up (Android MotionEvent action codes). */
int ain_touch_event(AndroidInput *, uint32_t action, uint32_t pointer,
                    uint32_t x, uint32_t y);
int ain_set_gamepad_state(AndroidInput *, const port_gamepad_state *);
int ain_set_surface_size(AndroidInput *, uint32_t width, uint32_t height);
int ain_focus_event(AndroidInput *, uint32_t gained);
int ain_close_event(AndroidInput *);
/* Releases every virtual key held by any source (focus loss, teardown). */
int ain_release_all(AndroidInput *);

/* --- static tables ----------------------------------------------------- */
uint32_t ain_dik_from_akeycode(int32_t akeycode);
uint32_t ain_vk_from_akeycode(int32_t akeycode);
uint32_t ain_dik_from_vk(uint32_t vk);
uint32_t ain_vk_from_name(const char *name);
const char *ain_vk_name(uint32_t vk);
uint32_t ain_akeycode_count(void);
int32_t ain_akeycode_at(uint32_t index, uint32_t *dik, uint32_t *vk);

/* --- touch zones ------------------------------------------------------- */
uint32_t ain_touch_zone(uint32_t x, uint32_t y, uint32_t width,
                        uint32_t height);
const char *ain_zone_name(uint32_t zone);
uint32_t ain_zone_vk(const AndroidInput *, uint32_t zone);
int ain_set_zone_vk(AndroidInput *, uint32_t zone, uint32_t vk);

/* --- gamepad -> keyboard table ----------------------------------------- */
const AinBinding *ain_bindings(const AndroidInput *, uint32_t *count);
int ain_set_binding_vk(AndroidInput *, uint32_t index, uint32_t vk);
int ain_load_bindings(AndroidInput *, const char *path);
const char *ain_bindings_path(const AndroidInput *);

/* --- diagnostics ------------------------------------------------------- */
const GdiHostState *ain_host_state(const AndroidInput *);
uint32_t ain_vk_holders(const AndroidInput *, uint32_t vk);
uint32_t ain_dik_state(const AndroidInput *, uint32_t dik);
uint32_t ain_events_posted(const AndroidInput *);
uint32_t ain_gamepad_updates(const AndroidInput *);
uint32_t ain_zone_hits(const AndroidInput *, uint32_t zone);
uint32_t ain_surface_width(const AndroidInput *);
uint32_t ain_surface_height(const AndroidInput *);
const char *ain_last_error(const AndroidInput *);

/* --- global hooks the front end looks up with dlsym -------------------- */
/* All three operate on one lazily created default instance; the image wires
 * it up with ain_default_set_targets() once the guest models exist. */
AndroidInput *ain_default_instance(void);
int ain_default_set_targets(GuestDinput *, GuestUser32 *);
int port_set_gamepad_state(const port_gamepad_state *state);
int port_post_host_input(uint32_t type, uint32_t x, uint32_t y, uint32_t code);
int port_set_surface_size(int width, int height);

#endif
