#ifndef BURNOUT_GUEST_BRIDGE_ABI_H
#define BURNOUT_GUEST_BRIDGE_ABI_H
#include <stdint.h>

/* Bridge ABI shared by the A8 window/message and COM/D3D11 modules.
 *
 * The startup harness (runtime/startup_host.c) implements its bridges as
 * `int run_x(uint32_t which, uint32_t *out)` and reads the Win32 arguments
 * from the guest stack itself. The A8 modules are not wired into that file
 * yet, so they expose a per-module export list instead:
 *
 *     { dll, name, stdcall_arg_bytes, fn }
 *
 * The host adapter is expected to copy the stdcall arguments from the guest
 * stack into a uint32_t array in *declaration order* (args[0] = first
 * argument, i.e. the value at [esp+4] on entry) and, after a successful call
 * (return value 0), to pop `stdcall_arg_bytes` from the guest stack and store
 * `*result` in EAX. `state` is the module instance (GuestUser32/GuestCom).
 *
 * Return value contract, identical to the existing bridges:
 *   0            call completed, *result holds the Win32 return value
 *   GM_* (>0)    guest-memory/model stop, *result is not a Win32 value
 *   GU_STOP_* (<0) named model stop, the guest must not continue
 */
typedef int (*GuBridgeFn)(void *state, const uint32_t *args, uint32_t *result);

/* Named model stops. Negative so they can never be confused with a Win32
 * BOOL/HRESULT or with the positive GM_* memory codes. Every stop has a
 * matching string in gu_stop_name() (guest_user32.c / guest_com.c). */
enum {
    GU_STOP_NO_GUEST_CALL = -2001,   /* no guest-call hook installed */
    GU_STOP_SECOND_WINDOW = -2002,   /* a second top-level window was created */
    GU_STOP_CHILD_WINDOW = -2003,    /* child/owned windows are not modelled */
    GU_STOP_MESSAGE_WAIT = -2004,    /* blocking wait without a host pump */
    GU_STOP_CURSOR_RESOURCE = -2005, /* named cursor resource, no resource tree */
    GU_STOP_ICON_RESOURCE = -2006,   /* named icon resource, no resource tree */
    GU_STOP_NON_ASCII = -2007,       /* only ASCII window/class text modelled */
    GU_STOP_NO_SURFACE = -2008,      /* host surface size not provided yet */
    GU_STOP_STRING_TOO_LONG = -2009,
    GU_STOP_UNKNOWN_WINDOW = -2010,  /* handle is not the modelled window */
    GU_STOP_COM_UNIMPLEMENTED = -2101, /* resolved COM method has no backend */
    GU_STOP_COM_BAD_OBJECT = -2102,  /* guest object/vtable is not registered */
    GU_STOP_COM_UNKNOWN_IID = -2103, /* interface has no documented IID here */
    GU_STOP_COM_NO_REGION = -2104,   /* no guest arena for COM objects */
    GU_STOP_COM_REGION_FULL = -2105,
    GU_STOP_COM_BAD_ARGS = -2106,
    GU_STOP_COM_ENTRY = -2107        /* D3D11/DXGI entry point parsed, no device */
};

/* Static inline so both A8 modules stay linkable on their own. */
static inline const char *gu_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_NO_GUEST_CALL: return "user32-no-guest-call-hook";
    case GU_STOP_SECOND_WINDOW: return "user32-second-top-level-window";
    case GU_STOP_CHILD_WINDOW: return "user32-child-window-unsupported";
    case GU_STOP_MESSAGE_WAIT: return "user32-blocking-message-wait-unsupported";
    case GU_STOP_CURSOR_RESOURCE: return "user32-named-cursor-resource-unsupported";
    case GU_STOP_ICON_RESOURCE: return "user32-named-icon-resource-unsupported";
    case GU_STOP_NON_ASCII: return "user32-non-ascii-text-unsupported";
    case GU_STOP_NO_SURFACE: return "user32-host-surface-size-missing";
    case GU_STOP_STRING_TOO_LONG: return "user32-string-too-long";
    case GU_STOP_UNKNOWN_WINDOW: return "user32-unknown-window-handle";
    case GU_STOP_COM_UNIMPLEMENTED: return "com-method-unimplemented";
    case GU_STOP_COM_BAD_OBJECT: return "com-unregistered-guest-object";
    case GU_STOP_COM_UNKNOWN_IID: return "com-interface-iid-unknown";
    case GU_STOP_COM_NO_REGION: return "com-no-guest-arena";
    case GU_STOP_COM_REGION_FULL: return "com-guest-arena-full";
    case GU_STOP_COM_BAD_ARGS: return "com-bad-arguments";
    case GU_STOP_COM_ENTRY: return "d3d11-entry-point-parsed-no-device";
    default: return "unknown-stop";
    }
}
#endif
