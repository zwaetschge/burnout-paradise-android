#ifndef BURNOUT_GUEST_XINPUT_H
#define BURNOUT_GUEST_XINPUT_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"

/* A19 -- XInput 1.3 bridges.
 *
 * The game resolves XInputGetState/XInputSetState by ordinal (2/3) through the
 * .srdata delay-load resolver; the ordinal->name mapping comes from the export
 * table of the reference xinput1_3.dll (runtime/ordinal_names.c), never from a
 * guess.
 *
 * The controller state is owned by the host (the Android input layer calls
 * gxi_set_host_state()). There is no Windows driver stack here, so:
 *   - an index that the host has not marked connected answers the documented
 *     ERROR_DEVICE_NOT_CONNECTED (1167) and writes nothing;
 *   - a connected index answers ERROR_SUCCESS (0) and fills the PE32 structure
 *     with the host state;
 *   - dwPacketNumber counts state changes for that index (it is not a clock);
 *   - XInputSetState forwards the two motor speeds to the optional host hook;
 *     without a hook the vibration request is accepted and counted, because a
 *     missing rumble motor is not an API error (documented XInput behaviour on
 *     a device without force feedback is ERROR_SUCCESS).
 *
 * PE32 structures (all little endian, no padding beyond the documented layout):
 *   XINPUT_STATE        16 bytes  dwPacketNumber(4) + XINPUT_GAMEPAD(12)
 *   XINPUT_GAMEPAD      12 bytes  wButtons(2) bLeftTrigger bRightTrigger
 *                                 sThumbLX(2) sThumbLY(2) sThumbRX(2) sThumbRY(2)
 *   XINPUT_VIBRATION     4 bytes  wLeftMotorSpeed(2) wRightMotorSpeed(2)
 *   XINPUT_CAPABILITIES 20 bytes  Type(1) SubType(1) Flags(2) Gamepad(12)
 *                                 Vibration(4)
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. A non-zero
 * function return is a model stop, never a Win32 status. */

#define GXI_MAX_CONTROLLERS 4u
#define GXI_STATE_SIZE 16u
#define GXI_VIBRATION_SIZE 4u
#define GXI_CAPABILITIES_SIZE 20u

#define GXI_ERROR_SUCCESS 0u
#define GXI_ERROR_DEVICE_NOT_CONNECTED 1167u
#define GXI_ERROR_BAD_ARGUMENTS 160u

#define GXI_DEVTYPE_GAMEPAD 1u
#define GXI_DEVSUBTYPE_GAMEPAD 1u
#define GXI_CAPS_FFB_SUPPORTED 0x0001u
#define GXI_CAPS_WIRELESS 0x0002u

/* XInputGetCapabilities dwFlags: 0 = any device, XINPUT_FLAG_GAMEPAD = 1. */
#define GXI_FLAG_GAMEPAD 1u

typedef struct GuestXinput GuestXinput;

int gxi_create(GuestMemory *, uint32_t teb, GuestXinput **out);
void gxi_destroy(GuestXinput *);

/* Host state injection (Android input layer / host tests). `connected` != 0
 * makes the index answer XInputGetState. Axes are the raw XInput ranges
 * (-32768..32767), triggers 0..255. */
int gxi_set_host_state(GuestXinput *, uint32_t index, uint32_t buttons,
                       uint32_t left_trigger, uint32_t right_trigger,
                       int32_t thumb_lx, int32_t thumb_ly, int32_t thumb_rx,
                       int32_t thumb_ry, int connected);
/* Optional rumble sink. Returns 0 on success. */
typedef int (*GxiVibrationHook)(void *context, uint32_t index, uint16_t left,
                                uint16_t right);
void gxi_set_vibration_hook(GuestXinput *, GxiVibrationHook, void *context);
/* XInputEnable(FALSE) suppresses state updates (documented: no input is
 * delivered while disabled). */
void gxi_set_enabled(GuestXinput *, int enabled);

/* Diagnostics (host tests only). */
uint32_t gxi_state_calls(const GuestXinput *);
uint32_t gxi_set_state_calls(const GuestXinput *);
uint32_t gxi_capability_calls(const GuestXinput *);
uint32_t gxi_not_connected(const GuestXinput *);
uint32_t gxi_vibration_calls(const GuestXinput *);
uint32_t gxi_packet_number(const GuestXinput *, uint32_t index);
int gxi_connected(const GuestXinput *, uint32_t index);

/* --- entry points (shared bridge signature) ---------------------------- */
int gxi_get_state(GuestXinput *, const uint32_t *args, uint32_t *result);
int gxi_set_state(GuestXinput *, const uint32_t *args, uint32_t *result);
int gxi_get_capabilities(GuestXinput *, const uint32_t *args,
                         uint32_t *result);
int gxi_enable(GuestXinput *, const uint32_t *args, uint32_t *result);

/* Registration list, wired by runtime/static_bridges.c. */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestXinput *, const uint32_t *args, uint32_t *result);
} GuestXinputExport;

extern const GuestXinputExport xinput1_3_exports[];
extern const uint32_t xinput1_3_export_count;

#endif
