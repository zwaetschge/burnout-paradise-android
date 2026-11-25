/* A19 -- XInput 1.3 bridges. See runtime/guest_xinput.h for the contract. */
#include "guest_xinput.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t buttons;
    uint32_t left_trigger;
    uint32_t right_trigger;
    int32_t thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    int connected;
    uint32_t packet;
    uint32_t last_buttons;
    uint32_t last_lt, last_rt;
    int32_t last_lx, last_ly, last_rx, last_ry;
} GxiPad;

struct GuestXinput {
    GuestMemory *mem;
    uint32_t teb;
    GxiPad pads[GXI_MAX_CONTROLLERS];
    int enabled;
    GxiVibrationHook hook;
    void *hook_context;
    uint32_t state_calls, set_state_calls, capability_calls;
    uint32_t not_connected, vibration_calls;
};

/* ---------------------------------------------------------------- helpers */

static void pack_gamepad(uint8_t out[12], const GxiPad *p) {
    uint16_t buttons = (uint16_t)p->buttons;
    uint16_t lx = (uint16_t)(int16_t)p->thumb_lx;
    uint16_t ly = (uint16_t)(int16_t)p->thumb_ly;
    uint16_t rx = (uint16_t)(int16_t)p->thumb_rx;
    uint16_t ry = (uint16_t)(int16_t)p->thumb_ry;
    out[0] = (uint8_t)buttons;  out[1] = (uint8_t)(buttons >> 8);
    out[2] = (uint8_t)p->left_trigger;
    out[3] = (uint8_t)p->right_trigger;
    out[4] = (uint8_t)lx;       out[5] = (uint8_t)(lx >> 8);
    out[6] = (uint8_t)ly;       out[7] = (uint8_t)(ly >> 8);
    out[8] = (uint8_t)rx;       out[9] = (uint8_t)(rx >> 8);
    out[10] = (uint8_t)ry;      out[11] = (uint8_t)(ry >> 8);
}

/* --------------------------------------------------------------- lifecycle */

int gxi_create(GuestMemory *mem, uint32_t teb, GuestXinput **out) {
    if (!mem || !out) return GM_RANGE;
    GuestXinput *xi = (GuestXinput *)calloc(1, sizeof *xi);
    if (!xi) return GM_ALLOCATION;
    xi->mem = mem;
    xi->teb = teb;
    xi->enabled = 1;
    *out = xi;
    return GM_OK;
}

void gxi_destroy(GuestXinput *xi) { free(xi); }

int gxi_set_host_state(GuestXinput *xi, uint32_t index, uint32_t buttons,
                       uint32_t left_trigger, uint32_t right_trigger,
                       int32_t thumb_lx, int32_t thumb_ly, int32_t thumb_rx,
                       int32_t thumb_ry, int connected) {
    if (!xi || index >= GXI_MAX_CONTROLLERS) return GM_RANGE;
    if (left_trigger > 255u || right_trigger > 255u) return GM_RANGE;
    if (thumb_lx < -32768 || thumb_lx > 32767 || thumb_ly < -32768 ||
        thumb_ly > 32767 || thumb_rx < -32768 || thumb_rx > 32767 ||
        thumb_ry < -32768 || thumb_ry > 32767)
        return GM_RANGE;
    GxiPad *p = &xi->pads[index];
    p->buttons = buttons & 0xffffu;
    p->left_trigger = left_trigger;
    p->right_trigger = right_trigger;
    p->thumb_lx = thumb_lx;
    p->thumb_ly = thumb_ly;
    p->thumb_rx = thumb_rx;
    p->thumb_ry = thumb_ry;
    p->connected = connected ? 1 : 0;
    return GM_OK;
}

void gxi_set_vibration_hook(GuestXinput *xi, GxiVibrationHook hook,
                            void *context) {
    if (!xi) return;
    xi->hook = hook;
    xi->hook_context = context;
}

void gxi_set_enabled(GuestXinput *xi, int enabled) {
    if (xi) xi->enabled = enabled ? 1 : 0;
}

/* ------------------------------------------------------------- diagnostics */

uint32_t gxi_state_calls(const GuestXinput *xi) { return xi ? xi->state_calls : 0; }
uint32_t gxi_set_state_calls(const GuestXinput *xi) { return xi ? xi->set_state_calls : 0; }
uint32_t gxi_capability_calls(const GuestXinput *xi) { return xi ? xi->capability_calls : 0; }
uint32_t gxi_not_connected(const GuestXinput *xi) { return xi ? xi->not_connected : 0; }
uint32_t gxi_vibration_calls(const GuestXinput *xi) { return xi ? xi->vibration_calls : 0; }
uint32_t gxi_packet_number(const GuestXinput *xi, uint32_t index) {
    if (!xi || index >= GXI_MAX_CONTROLLERS) return 0;
    return xi->pads[index].packet;
}
int gxi_connected(const GuestXinput *xi, uint32_t index) {
    if (!xi || index >= GXI_MAX_CONTROLLERS) return 0;
    return xi->pads[index].connected;
}

/* ------------------------------------------------------------ entry points */

/* XInputGetState(DWORD dwUserIndex, XINPUT_STATE *pState) */
int gxi_get_state(GuestXinput *xi, const uint32_t *args, uint32_t *result) {
    if (!xi) return GM_UNSUPPORTED;
    xi->state_calls++;
    uint32_t index = args[0], state = args[1];
    if (index >= GXI_MAX_CONTROLLERS) {
        xi->not_connected++;
        *result = GXI_ERROR_DEVICE_NOT_CONNECTED;
        return GM_OK;
    }
    GxiPad *p = &xi->pads[index];
    if (!p->connected) {
        xi->not_connected++;
        *result = GXI_ERROR_DEVICE_NOT_CONNECTED;
        return GM_OK;
    }
    if (!state) return GM_RANGE;
    int rc = gm_check(xi->mem, state, GXI_STATE_SIZE, GM_WRITE);
    if (rc) return rc;
    /* dwPacketNumber changes only when the delivered state changes. */
    int changed = p->buttons != p->last_buttons ||
                  p->left_trigger != p->last_lt ||
                  p->right_trigger != p->last_rt ||
                  p->thumb_lx != p->last_lx || p->thumb_ly != p->last_ly ||
                  p->thumb_rx != p->last_rx || p->thumb_ry != p->last_ry;
    if (changed) {
        p->packet++;
        p->last_buttons = p->buttons;
        p->last_lt = p->left_trigger;
        p->last_rt = p->right_trigger;
        p->last_lx = p->thumb_lx;
        p->last_ly = p->thumb_ly;
        p->last_rx = p->thumb_rx;
        p->last_ry = p->thumb_ry;
    }
    uint8_t bytes[GXI_STATE_SIZE];
    memset(bytes, 0, sizeof bytes);
    bytes[0] = (uint8_t)p->packet;
    bytes[1] = (uint8_t)(p->packet >> 8);
    bytes[2] = (uint8_t)(p->packet >> 16);
    bytes[3] = (uint8_t)(p->packet >> 24);
    /* While disabled XInput delivers a neutral state (documented: no input is
     * reported), but the packet number keeps counting. */
    if (xi->enabled) pack_gamepad(bytes + 4, p);
    rc = gm_write(xi->mem, state, bytes, GXI_STATE_SIZE);
    if (rc) return rc;
    *result = GXI_ERROR_SUCCESS;
    return GM_OK;
}

/* XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION *pVibration) */
int gxi_set_state(GuestXinput *xi, const uint32_t *args, uint32_t *result) {
    if (!xi) return GM_UNSUPPORTED;
    xi->set_state_calls++;
    uint32_t index = args[0], vibration = args[1];
    if (index >= GXI_MAX_CONTROLLERS || !xi->pads[index].connected) {
        xi->not_connected++;
        *result = GXI_ERROR_DEVICE_NOT_CONNECTED;
        return GM_OK;
    }
    if (!vibration) return GM_RANGE;
    uint8_t bytes[GXI_VIBRATION_SIZE];
    int rc = gm_read(xi->mem, vibration, bytes, GXI_VIBRATION_SIZE);
    if (rc) return rc;
    uint16_t left = (uint16_t)(bytes[0] | (bytes[1] << 8));
    uint16_t right = (uint16_t)(bytes[2] | (bytes[3] << 8));
    xi->vibration_calls++;
    if (xi->hook) {
        rc = xi->hook(xi->hook_context, index, left, right);
        if (rc) return GM_UNSUPPORTED;
    }
    /* No hook: the request is accepted (a device without motors is not an
     * XInput error) and only counted. Nothing is pretended to vibrate. */
    *result = GXI_ERROR_SUCCESS;
    return GM_OK;
}

/* XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags,
 *                       XINPUT_CAPABILITIES *pCapabilities) */
int gxi_get_capabilities(GuestXinput *xi, const uint32_t *args,
                         uint32_t *result) {
    if (!xi) return GM_UNSUPPORTED;
    xi->capability_calls++;
    uint32_t index = args[0], flags = args[1], caps = args[2];
    if (index >= GXI_MAX_CONTROLLERS || !xi->pads[index].connected) {
        xi->not_connected++;
        *result = GXI_ERROR_DEVICE_NOT_CONNECTED;
        return GM_OK;
    }
    if (flags != 0 && flags != GXI_FLAG_GAMEPAD) {
        /* Documented dwFlags values are 0 and XINPUT_FLAG_GAMEPAD; anything
         * else has no documented answer here, so it stops instead of guessing
         * ERROR_BAD_ARGUMENTS. */
        return GM_UNSUPPORTED;
    }
    if (!caps) return GM_RANGE;
    int rc = gm_check(xi->mem, caps, GXI_CAPABILITIES_SIZE, GM_WRITE);
    if (rc) return rc;
    uint8_t bytes[GXI_CAPABILITIES_SIZE];
    memset(bytes, 0, sizeof bytes);
    bytes[0] = GXI_DEVTYPE_GAMEPAD;
    bytes[1] = GXI_DEVSUBTYPE_GAMEPAD;
    /* Flags: force feedback is only advertised when a host hook exists. */
    uint16_t capflags = xi->hook ? GXI_CAPS_FFB_SUPPORTED : 0;
    bytes[2] = (uint8_t)capflags;
    bytes[3] = (uint8_t)(capflags >> 8);
    pack_gamepad(bytes + 4, &xi->pads[index]);
    if (xi->hook) {
        uint16_t full = 0xffffu;
        bytes[16] = (uint8_t)full;
        bytes[17] = (uint8_t)(full >> 8);
        bytes[18] = (uint8_t)full;
        bytes[19] = (uint8_t)(full >> 8);
    }
    rc = gm_write(xi->mem, caps, bytes, GXI_CAPABILITIES_SIZE);
    if (rc) return rc;
    *result = GXI_ERROR_SUCCESS;
    return GM_OK;
}

/* XInputEnable(BOOL enable) -- VOID. */
int gxi_enable(GuestXinput *xi, const uint32_t *args, uint32_t *result) {
    if (!xi) return GM_UNSUPPORTED;
    gxi_set_enabled(xi, args[0] != 0);
    *result = 0;
    return GM_OK;
}

/* ------------------------------------------------------------ registration */

const GuestXinputExport xinput1_3_exports[] = {
    { "xinput1_3.dll", "XInputGetState", 8, gxi_get_state },
    { "xinput1_3.dll", "XInputSetState", 8, gxi_set_state },
    { "xinput1_3.dll", "XInputGetCapabilities", 12, gxi_get_capabilities },
    { "xinput1_3.dll", "XInputEnable", 4, gxi_enable },
};
const uint32_t xinput1_3_export_count =
    (uint32_t)(sizeof xinput1_3_exports / sizeof xinput1_3_exports[0]);
