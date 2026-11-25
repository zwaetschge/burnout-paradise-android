/*
 * Burnout Paradise Remastered -> Android arm64-v8a
 * A17: host <-> static-image contract.
 *
 * This header is the *documented* ABI between the Android front end
 * (android/jni/port_main.c, libport_main.so) and the statically recompiled
 * image (libburnout_static.so, built from port/upstream/pcrecomp output by
 * tools/static_full_build.py / tools/static_host_run.py --android).
 *
 * Rules
 * -----
 *  - Only `port_entry` is mandatory. Every other symbol is optional and is
 *    looked up with dlsym(); a missing symbol produces one logcat line and the
 *    front end keeps running. Nothing is stubbed silently and nothing is
 *    guessed.
 *  - All entry points are called from the Android main (looper) thread unless
 *    noted otherwise. `port_entry` itself runs on its own pthread with a
 *    16 MiB stack (see PORT_GAME_THREAD_STACK) because the lifted guest code
 *    needs a large host stack and must never run on the looper thread.
 *  - An ANativeWindow* is only ever handed to the image; the front end keeps
 *    ownership and keeps the EGL heartbeat alive until the image reports that
 *    it took the window over (port_set_native_window returning 0).
 *
 * No game code, asset or license data is part of this contract.
 */

#ifndef BURNOUT_PORT_HOST_API_H
#define BURNOUT_PORT_HOST_API_H

#include <stdint.h>

/* Version of this header. Bump when a struct layout or a symbol signature
 * changes; the front end logs it at start-up. */
#define PORT_HOST_API_ABI 1u

/* Host stack for the game thread. The lifted x86 code uses the host stack for
 * its own C frames (guest ESP is emulated separately), so this only has to
 * cover the runtime's C recursion depth; 16 MiB is the value required by the
 * task and is what the front end allocates. */
#define PORT_GAME_THREAD_STACK (16u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* 1. Mandatory symbol                                                */
/* ------------------------------------------------------------------ */
/* int port_entry(const char *game_root, const char *user_root);
 *
 *   game_root : host directory the guest C:\BurnoutPR maps to (contains the
 *               user's own BurnoutPR.exe and game data, read-only).
 *   user_root : writable host directory for the guest user profile.
 *
 * Returns 0 only when the translated entry point returned normally. Every
 * unimplemented instruction/API ends as a named trap: one
 * `PORT_TRAP {json}` line on stderr followed by exit(70). */
#define PORT_SYM_ENTRY "port_entry"
/* int port_entry_ex(const char *game_root, const char *user_root,
 *                   const char *nls_path);
 * Optional variant that also receives a host path to the private c_1252.nls
 * bytes; used when the front end can ship/park them. NULL = ANSI code page
 * entry points stop instead of guessing bytes. */
#define PORT_SYM_ENTRY_EX "port_entry_ex"

/* ------------------------------------------------------------------ */
/* 2. Optional window / surface hand-over                             */
/* ------------------------------------------------------------------ */
/* int port_set_native_window(ANativeWindow *window, int width, int height);
 *   window == NULL means "the window is going away"; the image must drop every
 *   reference it holds. Return 0 when the image took the window over (the
 *   front end then stops drawing its own EGL heartbeat), non-zero when it did
 *   not (front end keeps the heartbeat). */
#define PORT_SYM_SET_NATIVE_WINDOW "port_set_native_window"

/* int port_set_surface_size(int width, int height);
 * Size-only notification (no window). Returns 0 on success. */
#define PORT_SYM_SET_SURFACE_SIZE "port_set_surface_size"

/* Fallback path used when the image does not export the two symbols above but
 * does export the project-runtime user32 model (runtime/guest_user32.h):
 *   int gu_set_surface_size(GuestUser32 *user32, uint32_t w, uint32_t h);
 *   int gu_post_host_input(GuestUser32 *user32, const GuHostInput *in);
 * The GuestUser32* is read from the image's exported data symbol "g_user32"
 * (a pointer variable, resolved with dlsym and dereferenced once per call).
 * Both are only used when the data symbol resolves to a non-NULL value. */
#define PORT_SYM_GU_SET_SURFACE_SIZE "gu_set_surface_size"
#define PORT_SYM_GU_POST_HOST_INPUT "gu_post_host_input"
#define PORT_SYM_GU_USER32_DATA "g_user32"

/* Mirror of runtime/guest_user32.h GuHostInput (6 x uint32_t = 24 bytes).
 * Kept as a plain struct here so the front end does not have to include the
 * whole guest runtime; the layout is asserted at build time in port_main.c. */
typedef struct port_gu_host_input {
    uint32_t type;        /* PORT_GU_INPUT_* */
    uint32_t x, y;        /* client coordinates for touch events */
    uint32_t code;        /* virtual-key code for key events */
    uint32_t width, height; /* new surface size for resize */
} port_gu_host_input;

enum {
    PORT_GU_INPUT_TOUCH_DOWN = 0,
    PORT_GU_INPUT_TOUCH_UP = 1,
    PORT_GU_INPUT_TOUCH_MOVE = 2,
    PORT_GU_INPUT_KEY_DOWN = 3,
    PORT_GU_INPUT_KEY_UP = 4,
    PORT_GU_INPUT_RESIZE = 5,
    PORT_GU_INPUT_CLOSE = 6,
    PORT_GU_INPUT_FOCUS_GAINED = 7,
    PORT_GU_INPUT_FOCUS_LOST = 8
};

/* Handle-free input entry point (preferred over the gu_* fallback):
 *   int port_post_host_input(uint32_t type, uint32_t x, uint32_t y,
 *                            uint32_t code);
 *   type = PORT_GU_INPUT_*, returns 0 when the event was queued. */
#define PORT_SYM_POST_HOST_INPUT "port_post_host_input"

/* ------------------------------------------------------------------ */
/* 3. Optional gamepad state                                          */
/* ------------------------------------------------------------------ */
/* int port_set_gamepad_state(const port_gamepad_state *state);
 * Called on every gamepad key/axis event and once with connected = 0 when the
 * last pad disappears. Returns 0 when the image accepted the state.
 *
 * Button bits (PORT_PAD_*): XInput-compatible ordering, so an image that maps
 * them onto XInputGetState only has to copy the mask. Axis values are
 * normalised to [-1.0, +1.0] with the documented deadzone *not* applied (the
 * image decides); triggers are [0.0, 1.0]. */
#define PORT_SYM_SET_GAMEPAD_STATE "port_set_gamepad_state"

enum {
    PORT_PAD_DPAD_UP = 0,
    PORT_PAD_DPAD_DOWN,
    PORT_PAD_DPAD_LEFT,
    PORT_PAD_DPAD_RIGHT,
    PORT_PAD_START,
    PORT_PAD_BACK,
    PORT_PAD_LEFT_THUMB,
    PORT_PAD_RIGHT_THUMB,
    PORT_PAD_LEFT_SHOULDER,
    PORT_PAD_RIGHT_SHOULDER,
    PORT_PAD_A,
    PORT_PAD_B,
    PORT_PAD_X,
    PORT_PAD_Y,
    PORT_PAD_BUTTON_COUNT
};

enum {
    PORT_AXIS_LEFT_X = 0,
    PORT_AXIS_LEFT_Y,
    PORT_AXIS_RIGHT_X,
    PORT_AXIS_RIGHT_Y,
    PORT_AXIS_LEFT_TRIGGER,
    PORT_AXIS_RIGHT_TRIGGER,
    PORT_AXIS_HAT_X,
    PORT_AXIS_HAT_Y,
    PORT_AXIS_COUNT
};

typedef struct port_gamepad_state {
    uint32_t abi_version;   /* PORT_HOST_API_ABI */
    uint32_t connected;     /* 1 = at least one pad is connected */
    uint32_t buttons;       /* bitmask, bit n = PORT_PAD_* n */
    uint32_t dpad;          /* bitmask of the four PORT_PAD_DPAD_* bits */
    float axis[PORT_AXIS_COUNT];
    uint32_t axis_count;    /* == PORT_AXIS_COUNT for this ABI */
    uint32_t reserved[3];   /* 0 */
} port_gamepad_state;

/* ------------------------------------------------------------------ */
/* 4. Optional lifecycle hooks                                        */
/* ------------------------------------------------------------------ */
/* int port_on_lifecycle(uint32_t event, void *data);
 *   PORT_LIFECYCLE_WINDOW_CREATED : data = ANativeWindow*, data2 in width/height
 *   PORT_LIFECYCLE_WINDOW_DESTROYED
 *   PORT_LIFECYCLE_PAUSE / _RESUME
 *   PORT_LIFECYCLE_FOCUS_GAINED / _LOST
 *   PORT_LIFECYCLE_DESTROY  (last call before the activity goes away)
 * Return 0 when handled. The front end never blocks on it. */
#define PORT_SYM_ON_LIFECYCLE "port_on_lifecycle"

enum {
    PORT_LIFECYCLE_WINDOW_CREATED = 1,
    PORT_LIFECYCLE_WINDOW_DESTROYED = 2,
    PORT_LIFECYCLE_PAUSE = 3,
    PORT_LIFECYCLE_RESUME = 4,
    PORT_LIFECYCLE_FOCUS_GAINED = 5,
    PORT_LIFECYCLE_FOCUS_LOST = 6,
    PORT_LIFECYCLE_DESTROY = 7
};

/* Per-event fallbacks, tried when port_on_lifecycle is absent:
 *   void port_on_pause(void); void port_on_resume(void);
 *   void port_on_window_destroy(void); */
#define PORT_SYM_ON_PAUSE "port_on_pause"
#define PORT_SYM_ON_RESUME "port_on_resume"
#define PORT_SYM_ON_WINDOW_DESTROY "port_on_window_destroy"

/* ------------------------------------------------------------------ */
/* 5. Windows virtual-key codes used by the input mapper              */
/* ------------------------------------------------------------------ */
/* The front end translates Android keycodes into Win32 virtual-key codes
 * before calling port_post_host_input()/gu_post_host_input(), because the
 * guest side only ever sees VK codes. Gamepad buttons use the XInput VK_PAD_*
 * range (0xC001..) that Windows itself defines. */
#define PORT_VK_BACK 0x08u
#define PORT_VK_TAB 0x09u
#define PORT_VK_RETURN 0x0Du
#define PORT_VK_SHIFT 0x10u
#define PORT_VK_CONTROL 0x11u
#define PORT_VK_MENU 0x12u  /* Alt */
#define PORT_VK_PAUSE 0x13u
#define PORT_VK_ESCAPE 0x1Bu
#define PORT_VK_SPACE 0x20u
#define PORT_VK_PRIOR 0x21u /* Page Up */
#define PORT_VK_NEXT 0x22u  /* Page Down */
#define PORT_VK_END 0x23u
#define PORT_VK_HOME 0x24u
#define PORT_VK_LEFT 0x25u
#define PORT_VK_UP 0x26u
#define PORT_VK_RIGHT 0x27u
#define PORT_VK_DOWN 0x28u
#define PORT_VK_INSERT 0x2Du
#define PORT_VK_DELETE 0x2Eu
#define PORT_VK_PAD_A 0xC001u
#define PORT_VK_PAD_B 0xC002u
#define PORT_VK_PAD_X 0xC003u
#define PORT_VK_PAD_Y 0xC004u
#define PORT_VK_PAD_RIGHT_SHOULDER 0xC005u
#define PORT_VK_PAD_LEFT_SHOULDER 0xC006u
#define PORT_VK_PAD_LEFT_TRIGGER 0xC007u
#define PORT_VK_PAD_RIGHT_TRIGGER 0xC008u
#define PORT_VK_PAD_DPAD_UP 0xC009u
#define PORT_VK_PAD_DPAD_DOWN 0xC00Au
#define PORT_VK_PAD_DPAD_LEFT 0xC00Bu
#define PORT_VK_PAD_DPAD_RIGHT 0xC00Cu
#define PORT_VK_PAD_START 0xC00Du
#define PORT_VK_PAD_BACK 0xC00Eu
#define PORT_VK_PAD_LEFT_THUMB 0xC00Fu
#define PORT_VK_PAD_RIGHT_THUMB 0xC010u

#endif /* BURNOUT_PORT_HOST_API_H */
