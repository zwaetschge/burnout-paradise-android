#ifndef BURNOUT_GUEST_USER32_H
#define BURNOUT_GUEST_USER32_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"

/* user32 window/message model for the single guest process.
 *
 * Exactly one top-level game window is modelled and it is mapped onto one
 * host surface whose size the host provides through gu_set_surface_size()
 * (on Android: the ANativeWindow size). There is no desktop, no window
 * manager, no child/owned window, no resource tree, no GDI DC and no
 * synthesised input: host input arrives through gu_post_host_input().
 *
 * Every Win32 API writes its return value to *result and its error to the
 * guest TEB+0x34 (LastError). A non-zero function return is a model stop
 * (GM_* memory code or GU_STOP_* named stop), never a Win32 status.
 *
 * Guest structures are written field by field through gm_write(); no host
 * pointer is ever stored in guest memory. */

#define GU_HWND_MAIN 0x00010001u
#define GU_MAX_CLASSES 16u
#define GU_MAX_MESSAGES 256u
#define GU_CLASS_NAME_MAX 64u
#define GU_WINDOW_TEXT_MAX 128u

/* Guest window handle for the one modelled window; 0 when none exists. */
#define GU_HWND_NONE 0u

/* Message queue record: PE32 MSG, 28 bytes. */
typedef struct {
    uint32_t hwnd, message, wparam, lparam, time, pt_x, pt_y;
} GuMessage;

/* Guest call hook: executes `address` as a guest stdcall function with
 * `count` uint32_t arguments and stores the guest EAX in *result. The host
 * adapter owns the guest CPU; without a hook every API that must deliver a
 * message to the guest WndProc returns GU_STOP_NO_GUEST_CALL. */
typedef int (*GuGuestCall)(void *context, uint32_t address,
                           const uint32_t *args, uint32_t count, uint32_t *result);
/* Optional blocking pump for GetMessage. Returns 1 when at least one message
 * was queued, 0 when the guest should observe WM_QUIT, or a negative stop. */
typedef int (*GuMessageWait)(void *context, uint32_t milliseconds);

typedef struct {
    uint32_t type;   /* GU_INPUT_* */
    uint32_t x, y;   /* client coordinates for touch events */
    uint32_t code;   /* virtual-key code for key events */
    uint32_t width, height; /* new surface size for GU_INPUT_RESIZE */
} GuHostInput;

enum {
    GU_INPUT_TOUCH_DOWN = 0,
    GU_INPUT_TOUCH_UP = 1,
    GU_INPUT_TOUCH_MOVE = 2,
    GU_INPUT_KEY_DOWN = 3,
    GU_INPUT_KEY_UP = 4,
    GU_INPUT_RESIZE = 5,
    GU_INPUT_CLOSE = 6,
    GU_INPUT_FOCUS_GAINED = 7,
    GU_INPUT_FOCUS_LOST = 8
};

typedef struct GuestUser32 GuestUser32;

GuestUser32 *gu_create(GuestMemory *memory, uint32_t teb);
void gu_destroy(GuestUser32 *user32);

/* Host side configuration. All three are optional but a window needs a
 * surface size and a guest-call hook. */
int gu_set_surface_size(GuestUser32 *user32, uint32_t width, uint32_t height);
int gu_set_guest_call(GuestUser32 *user32, GuGuestCall call, void *context);
int gu_set_message_wait(GuestUser32 *user32, GuMessageWait wait, void *context);
/* Optional guest scratch block for CREATESTRUCTA delivered with WM_NCCREATE.
 * Without it WM_NCCREATE carries lParam = 0 (documented deviation: no guest
 * heap allocator exists in this module). */
int gu_set_scratch(GuestUser32 *user32, uint32_t address, uint32_t size);

/* Host input -> WM_* messages (thread safe). */
int gu_post_host_input(GuestUser32 *user32, const GuHostInput *input);

/* Diagnostics for the host and the tests. */
uint32_t gu_message_count(GuestUser32 *user32);
uint32_t gu_window_count(GuestUser32 *user32);
uint32_t gu_hwnd(GuestUser32 *user32);
int gu_window_visible(GuestUser32 *user32);
uint32_t gu_quit_code(GuestUser32 *user32);
int gu_quit_posted(GuestUser32 *user32);
int gu_cursor_counter(GuestUser32 *user32);
uint32_t gu_cursor(GuestUser32 *user32);
uint32_t gu_surface_width(GuestUser32 *user32);
uint32_t gu_surface_height(GuestUser32 *user32);
uint32_t gu_class_count(GuestUser32 *user32);
/* GetSystemMetrics calls served so far (unknown indices answer the documented
 * "metric not present" 0 and are counted here). */
uint32_t gu_metrics_calls(GuestUser32 *user32);
/* Last MessageBoxA/W text, copied out for logging/tests (ASCII, NUL padded). */
int gu_last_message_box(GuestUser32 *user32, char *text, uint32_t capacity);

/* --- Win32 entry points ------------------------------------------------ */
int gu_register_class_ex_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_register_class_ex_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_create_window_ex_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_create_window_ex_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_def_window_proc_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_def_window_proc_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_peek_message_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_peek_message_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_message_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_message_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_translate_message(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_dispatch_message_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_dispatch_message_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_post_quit_message(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_show_window(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_update_window(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_client_rect(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_window_rect(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_adjust_window_rect(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_adjust_window_rect_ex(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_set_window_long_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_set_window_long_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_window_long_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_window_long_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_get_system_metrics(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_enum_display_settings_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_enum_display_settings_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_show_cursor(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_set_cursor(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_load_cursor_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_load_cursor_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_load_icon_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_load_icon_w(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_message_box_a(GuestUser32 *, const uint32_t *args, uint32_t *result);
int gu_message_box_w(GuestUser32 *, const uint32_t *args, uint32_t *result);

/* Registration list for the host adapter (wired later, see guest_bridge_abi.h).
 * `stdcall_arg_bytes` excludes the return address; the adapter pops it. */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestUser32 *, const uint32_t *args, uint32_t *result);
} GuUser32Export;

extern const GuUser32Export gu_user32_exports[];
extern const uint32_t gu_user32_export_count;

#endif
