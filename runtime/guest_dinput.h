#ifndef BURNOUT_GUEST_DINPUT_H
#define BURNOUT_GUEST_DINPUT_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"

/* DirectInput8 guest COM objects.
 *
 * The static import census shows exactly one dinput8 entry point
 * (DirectInput8Create) and GUID_SysKeyboard in the image, so the port needs a
 * real IDirectInput8/IDirectInputDevice8 object pair, not a stub.
 *
 * Guest objects live in guest memory: a 16-byte block whose first dword is the
 * guest address of its vtable. Vtable entries are guest addresses inside the
 * reserved trap range 0x6e000000..0x6e800000; the host resolves them back to
 * (interface, slot) and either executes the method here or reports a named stop
 * ("dinput-unimplemented: <Interface>::<Method>"). No host pointer is ever
 * written into guest memory.
 *
 * Implemented methods:
 *   IUnknown            QueryInterface / AddRef / Release
 *   IDirectInput8A/W    CreateDevice, EnumDevices, GetDeviceStatus
 *   IDirectInputDevice8A/W
 *                       Acquire, Unacquire, GetDeviceState, SetDataFormat,
 *                       SetCooperativeLevel, Poll, GetDeviceInfo,
 *                       GetCapabilities, GetProperty, SetProperty
 * Everything else is a named stop.
 *
 * Input comes from a host state block that the Android input layer fills
 * (gdi_set_host_state): 256 keyboard bytes, mouse deltas/buttons, joystick
 * axes/POV/buttons. Device data is copied into the guest buffer with the
 * documented c_dfDIKeyboard (256) / DIMOUSESTATE (16) / DIJOYSTATE (72) /
 * DIJOYSTATE2 (272) sizes; the guest's DIDATAFORMAT.dwDataSize decides how much
 * is written.
 *
 * Documented limits: no force feedback, no effects, no action maps, no
 * exclusive/background cooperative level, no device property store beyond
 * DIPROP_BUFFERSIZE, no buffered data (GetDeviceData is a named stop), and the
 * joystick instance GUID is the single host gamepad rather than a vendor GUID
 * read from a device. */

#define GDI_TRAP_BASE 0x6e000000u
#define GDI_TRAP_STRIDE 16u
#define GDI_TRAP_LIMIT 0x6e800000u
#define GDI_OBJECT_SIZE 16u
#define GDI_MAGIC 0x31444947u /* "GID1" */

#define GDI_S_OK 0x00000000u
#define GDI_S_FALSE 0x00000001u
#define GDI_DI_OK 0x00000000u
#define GDI_DI_NOEFFECT 0x00000001u
#define GDI_DI_NOTATTACHED 0x00000001u
#define GDI_DIERR_INVALIDPARAM 0x80070057u
#define GDI_DIERR_NOTINITIALIZED 0x80070015u
#define GDI_DIERR_NOTACQUIRED 0x8007000cu
#define GDI_DIERR_UNSUPPORTED 0x80070032u
#define GDI_DIERR_DEVICENOTREG 0x80070054u
#define GDI_DIERR_OLDDIRECTINPUTVERSION 0x80070486u
#define GDI_DIERR_NOEFFECT 0x80070001u
#define GDI_E_NOINTERFACE 0x80004002u
#define GDI_E_INVALIDARG 0x80070057u
#define GDI_CLASS_E_NOAGGREGATION 0x80040110u

#define GDI_DIRECTINPUT_VERSION 0x0800u
#define GDI_DIENUM_STOP 0u
#define GDI_DIENUM_CONTINUE 1u

/* Device classes / types. */
#define GDI_DI8DEVCLASS_ALL 0u
#define GDI_DI8DEVCLASS_POINTER 2u
#define GDI_DI8DEVCLASS_KEYBOARD 3u
#define GDI_DI8DEVCLASS_GAMECTRL 4u
#define GDI_DI8DEVTYPE_DEVICE 0x11u
#define GDI_DI8DEVTYPE_MOUSE 0x12u
#define GDI_DI8DEVTYPE_KEYBOARD 0x13u
#define GDI_DI8DEVTYPE_JOYSTICK 0x14u
#define GDI_DI8DEVTYPE_GAMEPAD 0x15u

/* Cooperative levels. */
#define GDI_DISCL_EXCLUSIVE 0x00000001u
#define GDI_DISCL_NONEXCLUSIVE 0x00000002u
#define GDI_DISCL_FOREGROUND 0x00000004u
#define GDI_DISCL_BACKGROUND 0x00000008u

/* Data-format sizes. */
#define GDI_KEYBOARD_BYTES 256u
#define GDI_MOUSE_BYTES 16u
#define GDI_JOYSTATE_BYTES 72u
#define GDI_JOYSTATE2_BYTES 272u
#define GDI_DIDATAFORMAT_SIZE 28u
#define GDI_DIDEVCAPS_SIZE 44u
#define GDI_DIDEVICEINSTANCE_A_SIZE 580u
#define GDI_DIDEVICEINSTANCE_W_SIZE 1100u

/* Host input state; filled by the Android input layer. */
typedef struct {
    uint8_t keys[256];
    uint32_t mouse_buttons;
    int32_t mouse_x, mouse_y, mouse_z;
    int32_t axis[6];   /* lX,lY,lZ,lRx,lRy,lRz */
    uint32_t pov[4];
    uint8_t buttons[128];
    uint32_t present;  /* bit 0 keyboard, 1 mouse, 2 gamepad */
    uint32_t gamepad_type; /* GDI_DI8DEVTYPE_JOYSTICK or _GAMEPAD */
} GdiHostState;

/* Module-local named stops. */
enum {
    GU_STOP_DI_UNIMPLEMENTED = -2701,
    GU_STOP_DI_BAD_OBJECT = -2702,
    GU_STOP_DI_NO_REGION = -2703,
    GU_STOP_DI_REGION_FULL = -2704,
    GU_STOP_DI_BAD_ARGS = -2705,
    GU_STOP_DI_NO_GUEST_CALL = -2706,
    GU_STOP_DI_FORMAT = -2707,
    GU_STOP_DI_DEVICE_TYPE = -2708
};

typedef struct GuestDinput GuestDinput;

int gdi_create(GuestMemory *, uint32_t teb, GuestDinput **out);
void gdi_destroy(GuestDinput *);
/* Guest arena for vtables/objects (host-mapped, writable). */
int gdi_set_arena(GuestDinput *, uint32_t base, uint32_t size);
void gdi_set_guest_call(GuestDinput *, int (*call)(void *, uint32_t,
                                                   const uint32_t *, uint32_t,
                                                   uint32_t *),
                        void *context);
/* Which DIK codes the platform keyboard has (256 flags); EnumObjects on the
 * keyboard device reports exactly these keys. */
void gdi_set_keyboard_keys(GuestDinput *, const uint8_t *present);
/* Identity of the platform game controller as DirectInput reports it:
 * guidProduct {vidpid-0000-0000-0000-504944564944}, instance/product name
 * `name`, device type "HID game pad". Without it the controller is the
 * anonymous placeholder device. */
int gdi_set_gamepad_identity(GuestDinput *, uint32_t vidpid, const char *name);
/* Host input. The state is copied; the caller keeps ownership. */
int gdi_set_host_state(GuestDinput *, const GdiHostState *);
int gdi_host_state(const GuestDinput *, GdiHostState *);

/* Trap dispatch: the host calls this when the guest jumps to a vtable entry. */
int gdi_call(GuestDinput *, uint32_t trap_address, const uint32_t *args,
             uint32_t *result);
int gdi_resolve(GuestDinput *, uint32_t address, uint32_t *interface_index,
                uint32_t *slot);
uint32_t gdi_interface_count(void);
const char *gdi_interface_name(uint32_t index);
uint32_t gdi_method_count(uint32_t interface_index);
uint32_t gdi_method_arg_bytes(uint32_t interface_index, uint32_t slot);
uint32_t gdi_method_trap(uint32_t interface_index, uint32_t slot);

/* Diagnostics. */
uint32_t gdi_live_objects(const GuestDinput *);
uint32_t gdi_refcount(const GuestDinput *, uint32_t object);
const char *gdi_last_stop(const GuestDinput *);
const char *gdi_stop_name(int code);

/* --- entry points ------------------------------------------------------ */
int gdi_direct_input8_create(GuestDinput *, const uint32_t *args,
                             uint32_t *result);

typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestDinput *, const uint32_t *args, uint32_t *result);
} GuestDinputExport;

extern const GuestDinputExport dinput8_exports[];
extern const uint32_t dinput8_export_count;

#endif
