/* DirectInput8 guest COM objects. See guest_dinput.h for the modelled scope.
 *
 * Only the documented methods are executed; every other slot is a named stop,
 * so an unimplemented method can never look like a successful HRESULT. Guest
 * memory is only touched through gm_check/gm_read/gm_write. */
#include "guest_dinput.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- interface tables ---------------------------------------------------- */
typedef struct {
    const char *name;
    uint32_t arg_bytes; /* stdcall bytes including `this` */
} GdiMethod;

typedef struct {
    const char *name;
    uint8_t iid[16];
    const GdiMethod *methods;
    uint32_t method_count;
    int device; /* 0 = IDirectInput8, 1 = IDirectInputDevice8 */
    int wide;   /* 1 = W variant */
} GdiInterface;

#define GDI_M(name, args) { name, 4u + 4u * (args) }

static const GdiMethod directinput8_methods[] = {
    GDI_M("QueryInterface", 2), GDI_M("AddRef", 0), GDI_M("Release", 0),
    GDI_M("CreateDevice", 3), GDI_M("EnumDevices", 4),
    GDI_M("GetDeviceStatus", 1), GDI_M("RunControlPanel", 2),
    GDI_M("Initialize", 2), GDI_M("FindDevice", 3),
    GDI_M("EnumDevicesBySemantics", 5), GDI_M("ConfigureDevices", 4)
};
static const GdiMethod device8_methods[] = {
    GDI_M("QueryInterface", 2), GDI_M("AddRef", 0), GDI_M("Release", 0),
    GDI_M("GetCapabilities", 1), GDI_M("EnumObjects", 3),
    GDI_M("GetProperty", 2), GDI_M("SetProperty", 2), GDI_M("Acquire", 0),
    GDI_M("Unacquire", 0), GDI_M("GetDeviceState", 2),
    GDI_M("GetDeviceData", 4), GDI_M("SetDataFormat", 1),
    GDI_M("SetEventNotification", 1), GDI_M("SetCooperativeLevel", 2),
    GDI_M("GetObjectInfo", 3), GDI_M("GetDeviceInfo", 1),
    GDI_M("RunControlPanel", 2), GDI_M("Initialize", 3),
    GDI_M("CreateEffect", 4), GDI_M("EnumEffects", 3),
    GDI_M("GetEffectInfo", 2), GDI_M("GetForceFeedbackState", 1),
    GDI_M("SendForceFeedbackCommand", 1), GDI_M("EnumCreatedEffectObjects", 3),
    GDI_M("Escape", 1), GDI_M("Poll", 0), GDI_M("SendDeviceData", 4),
    GDI_M("EnumEffectsInFile", 4), GDI_M("WriteEffectToFile", 4),
    GDI_M("BuildActionMap", 3), GDI_M("SetActionMap", 3),
    GDI_M("GetImageInfo", 1)
};

/* 16-byte little-endian GUID literal: Data1 (4), Data2 (2), Data3 (2),
 * Data4 (8 bytes). */
#define GDI_IID(a, b, c, d1, d2, d3, d4, d5, d6, d7, d8) \
    { (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), \
      (uint8_t)((a) >> 24), (uint8_t)(b), (uint8_t)((b) >> 8), \
      (uint8_t)(c), (uint8_t)((c) >> 8), \
      (uint8_t)(d1), (uint8_t)(d2), (uint8_t)(d3), (uint8_t)(d4), \
      (uint8_t)(d5), (uint8_t)(d6), (uint8_t)(d7), (uint8_t)(d8) }

static const GdiInterface gdi_interfaces[] = {
    { "IDirectInput8A",
      /* {BF79803x-483A-4DA2-AA99-5D64ED369700}: dinput.h; the W value is
       * what the original image passes to DirectInput8Create (0x00A3539C). */
      GDI_IID(0xbf798030u, 0x483au, 0x4da2u, 0xaa, 0x99, 0x5d, 0x64, 0xed,
              0x36, 0x97, 0x00),
      directinput8_methods,
      sizeof(directinput8_methods) / sizeof(directinput8_methods[0]), 0, 0 },
    { "IDirectInput8W",
      /* {BF79803x-483A-4DA2-AA99-5D64ED369700}: dinput.h; the W value is
       * what the original image passes to DirectInput8Create (0x00A3539C). */
      GDI_IID(0xbf798031u, 0x483au, 0x4da2u, 0xaa, 0x99, 0x5d, 0x64, 0xed,
              0x36, 0x97, 0x00),
      directinput8_methods,
      sizeof(directinput8_methods) / sizeof(directinput8_methods[0]), 0, 1 },
    { "IDirectInputDevice8A",
      GDI_IID(0x54d41080u, 0xdc15u, 0x4833u, 0xa4, 0x1b, 0x74, 0x8f, 0x73,
              0xa3, 0x81, 0x79),
      device8_methods,
      sizeof(device8_methods) / sizeof(device8_methods[0]), 1, 0 },
    { "IDirectInputDevice8W",
      GDI_IID(0x54d41081u, 0xdc15u, 0x4833u, 0xa4, 0x1b, 0x74, 0x8f, 0x73,
              0xa3, 0x81, 0x79),
      device8_methods,
      sizeof(device8_methods) / sizeof(device8_methods[0]), 1, 1 }
};
#define GDI_INTERFACE_COUNT \
    ((uint32_t)(sizeof(gdi_interfaces) / sizeof(gdi_interfaces[0])))

/* Documented GUIDs. */
static const uint8_t gdi_iunknown[16] = GDI_IID(0x00000000u, 0x0000u, 0x0000u,
                                                0xc0, 0x00, 0x00, 0x00, 0x00,
                                                0x00, 0x00, 0x46);
static const uint8_t gdi_sys_keyboard[16] = GDI_IID(
    0x6f1d2b61u, 0xd5a0u, 0x11cfu, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00,
    0x00);
static const uint8_t gdi_sys_mouse[16] = GDI_IID(
    0x6f1d2b60u, 0xd5a0u, 0x11cfu, 0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00,
    0x00);
static const uint8_t gdi_prop_buffersize[16] = GDI_IID(
    0x00000001u, 0x0000u, 0x0000u, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x46);

/* --- state --------------------------------------------------------------- */
typedef struct {
    int used;
    uint32_t object;
    uint32_t interface_index;
    uint32_t refcount;
    /* device-only state */
    uint32_t device_type;   /* 0 keyboard, 1 mouse, 2 joystick */
    uint32_t data_size;
    uint32_t cooperative;
    uint32_t acquired;
    uint32_t has_format;
} GdiObject;

struct GuestDinput {
    GuestMemory *memory;
    uint32_t teb;
    uint32_t arena_base, arena_size, arena_used;
    GdiObject objects[64];
    uint32_t live_objects;
    GdiHostState host;
    uint8_t key_present[256]; /* DIK codes the platform keyboard has */
    uint32_t pad_vidpid;      /* MAKELONG(vid, pid) of the game controller, 0 = none */
    char pad_name[64];
    int (*call)(void *, uint32_t, const uint32_t *, uint32_t, uint32_t *);
    void *call_context;
    char last_stop[128];
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
static int write_u32(GuestDinput *di, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(di->memory, address, b, 4);
}
static int stop(GuestDinput *di, int code, const char *what) {
    snprintf(di->last_stop, sizeof(di->last_stop), "%s", what);
    fprintf(stderr, "dinput-stop: %s (%d)\n", what, code);
    return code;
}

int gdi_create(GuestMemory *memory, uint32_t teb, GuestDinput **out) {
    if (!memory || !out) return GM_RANGE;
    GuestDinput *di = (GuestDinput *)calloc(1, sizeof(*di));
    if (!di) return GM_ALLOCATION;
    di->memory = memory;
    di->teb = teb;
    *out = di;
    return GM_OK;
}
void gdi_destroy(GuestDinput *di) { free(di); }
int gdi_set_arena(GuestDinput *di, uint32_t base, uint32_t size) {
    if (!di || size <= GDI_DIDEVICEINSTANCE_W_SIZE) return GU_STOP_DI_BAD_ARGS;
    int rc = gm_check(di->memory, base, size, GM_WRITE);
    if (rc) return rc;
    di->arena_base = base;
    /* The last DIDEVICEINSTANCEW slot is reserved for the EnumDevices scratch
     * block so it can never overlap a live vtable or object. */
    di->arena_size = size - GDI_DIDEVICEINSTANCE_W_SIZE;
    di->arena_used = 0;
    return GM_OK;
}
void gdi_set_guest_call(GuestDinput *di,
                        int (*call)(void *, uint32_t, const uint32_t *,
                                    uint32_t, uint32_t *),
                        void *context) {
    if (!di) return;
    di->call = call;
    di->call_context = context;
}
int gdi_set_host_state(GuestDinput *di, const GdiHostState *state) {
    if (!di || !state) return GU_STOP_DI_BAD_ARGS;
    di->host = *state;
    return GM_OK;
}
int gdi_host_state(const GuestDinput *di, GdiHostState *state) {
    if (!di || !state) return GU_STOP_DI_BAD_ARGS;
    *state = di->host;
    return GM_OK;
}
uint32_t gdi_live_objects(const GuestDinput *di) {
    return di ? di->live_objects : 0u;
}
uint32_t gdi_refcount(const GuestDinput *di, uint32_t object) {
    if (!di) return 0;
    for (uint32_t i = 0; i < 64; i++)
        if (di->objects[i].used && di->objects[i].object == object)
            return di->objects[i].refcount;
    return 0;
}
const char *gdi_last_stop(const GuestDinput *di) {
    return (di && di->last_stop[0]) ? di->last_stop : NULL;
}
const char *gdi_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_DI_UNIMPLEMENTED: return "dinput-method-unimplemented";
    case GU_STOP_DI_BAD_OBJECT: return "dinput-unregistered-guest-object";
    case GU_STOP_DI_NO_REGION: return "dinput-no-guest-arena";
    case GU_STOP_DI_REGION_FULL: return "dinput-guest-arena-full";
    case GU_STOP_DI_BAD_ARGS: return "dinput-argument-outside-model";
    case GU_STOP_DI_NO_GUEST_CALL: return "dinput-no-guest-call-hook";
    case GU_STOP_DI_FORMAT: return "dinput-data-format-unsupported";
    case GU_STOP_DI_DEVICE_TYPE: return "dinput-device-type-unsupported";
    default: return "unknown-stop";
    }
}

/* --- interface table access --------------------------------------------- */
uint32_t gdi_interface_count(void) { return GDI_INTERFACE_COUNT; }
const char *gdi_interface_name(uint32_t index) {
    return index < GDI_INTERFACE_COUNT ? gdi_interfaces[index].name : NULL;
}
uint32_t gdi_method_count(uint32_t interface_index) {
    if (interface_index >= GDI_INTERFACE_COUNT) return 0;
    return gdi_interfaces[interface_index].method_count;
}
uint32_t gdi_method_arg_bytes(uint32_t interface_index, uint32_t slot) {
    if (interface_index >= GDI_INTERFACE_COUNT) return 0;
    const GdiInterface *iface = &gdi_interfaces[interface_index];
    if (slot >= iface->method_count) return 0;
    return iface->methods[slot].arg_bytes;
}
static uint32_t global_index(uint32_t interface_index, uint32_t slot) {
    if (interface_index >= GDI_INTERFACE_COUNT) return UINT32_MAX;
    if (slot >= gdi_interfaces[interface_index].method_count) return UINT32_MAX;
    uint32_t base = 0;
    for (uint32_t i = 0; i < interface_index; i++)
        base += gdi_interfaces[i].method_count;
    return base + slot;
}
uint32_t gdi_method_trap(uint32_t interface_index, uint32_t slot) {
    uint32_t index = global_index(interface_index, slot);
    if (index == UINT32_MAX) return 0;
    return GDI_TRAP_BASE + index * GDI_TRAP_STRIDE;
}
int gdi_resolve(GuestDinput *di, uint32_t address, uint32_t *interface_index,
                uint32_t *slot) {
    (void)di;
    if (!interface_index || !slot) return GU_STOP_DI_BAD_ARGS;
    if (address < GDI_TRAP_BASE || address >= GDI_TRAP_LIMIT)
        return GU_STOP_DI_BAD_OBJECT;
    uint32_t delta = address - GDI_TRAP_BASE;
    if (delta % GDI_TRAP_STRIDE) return GU_STOP_DI_BAD_OBJECT;
    uint32_t index = delta / GDI_TRAP_STRIDE;
    for (uint32_t i = 0; i < GDI_INTERFACE_COUNT; i++) {
        uint32_t count = gdi_interfaces[i].method_count;
        if (index < count) { *interface_index = i; *slot = index; return 0; }
        index -= count;
    }
    return GU_STOP_DI_BAD_OBJECT;
}

/* --- guest objects ------------------------------------------------------ */
static GdiObject *find_object(GuestDinput *di, uint32_t object) {
    for (uint32_t i = 0; i < 64; i++)
        if (di->objects[i].used && di->objects[i].object == object)
            return &di->objects[i];
    return NULL;
}
static int create_object(GuestDinput *di, uint32_t interface_index,
                         uint32_t *object) {
    if (interface_index >= GDI_INTERFACE_COUNT) return GU_STOP_DI_BAD_ARGS;
    if (!di->arena_size) return GU_STOP_DI_NO_REGION;
    GdiObject *record = NULL;
    for (uint32_t i = 0; i < 64; i++)
        if (!di->objects[i].used) { record = &di->objects[i]; break; }
    if (!record) return GU_STOP_DI_REGION_FULL;
    const GdiInterface *iface = &gdi_interfaces[interface_index];
    uint32_t vtable_size = iface->method_count * 4u;
    uint32_t aligned = (vtable_size + 15u) & ~15u;
    uint32_t needed = aligned + GDI_OBJECT_SIZE;
    if (di->arena_used + needed > di->arena_size) return GU_STOP_DI_REGION_FULL;
    uint32_t vtable = di->arena_base + di->arena_used;
    uint32_t self = vtable + aligned;
    di->arena_used += needed;
    uint8_t entry[4];
    for (uint32_t slot = 0; slot < iface->method_count; slot++) {
        put32(entry, gdi_method_trap(interface_index, slot));
        int rc = gm_write(di->memory, vtable + slot * 4u, entry, 4);
        if (rc) return rc;
    }
    uint8_t block[GDI_OBJECT_SIZE];
    put32(block + 0, vtable);
    put32(block + 4, 1);
    put32(block + 8, interface_index);
    put32(block + 12, GDI_MAGIC);
    int rc = gm_write(di->memory, self, block, sizeof(block));
    if (rc) return rc;
    record->used = 1;
    record->object = self;
    record->interface_index = interface_index;
    record->refcount = 1;
    di->live_objects++;
    *object = self;
    return 0;
}

/* --- helpers ------------------------------------------------------------ */
static int iid_matches(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}
static int device_present(GuestDinput *di, uint32_t type) {
    if (type == 0) return (di->host.present & 1u) != 0;
    if (type == 1) return (di->host.present & 2u) != 0;
    return (di->host.present & 4u) != 0;
}
static uint32_t device_devtype(GuestDinput *di, uint32_t type) {
    if (type == 0) return GDI_DI8DEVTYPE_KEYBOARD;
    if (type == 1) return GDI_DI8DEVTYPE_MOUSE;
    return di->host.gamepad_type ? di->host.gamepad_type
                                 : GDI_DI8DEVTYPE_JOYSTICK;
}
/* Instance GUID of the single platform game controller: port-owned and fixed,
 * so the game's saved controller assignment stays valid between runs. */
static const uint8_t gdi_pad_instance[16] = GDI_IID(
    0x504f5254u, 0x5041u, 0x4431u, 0x80, 0x00, 0x42, 0x50, 0x41, 0x44, 0x30,
    0x31);
static const uint8_t *device_guid(uint32_t type) {
    if (type == 0) return gdi_sys_keyboard;
    if (type == 1) return gdi_sys_mouse;
    return gdi_pad_instance;
}
/* guidProduct: DirectInput's documented form for a HID device,
 * {MAKELONG(vid,pid)-0000-0000-0000-504944564944} ("PIDVID"). */
static void device_product_guid(GuestDinput *di, uint32_t type, uint8_t out[16]) {
    if (type != 2) { memcpy(out, device_guid(type), 16); return; }
    static const uint8_t tail[8] = { 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 };
    memset(out, 0, 16);
    put32(out, di->pad_vidpid ? di->pad_vidpid : 1u);
    memcpy(out + 8, tail, 8);
}
int gdi_set_gamepad_identity(GuestDinput *di, uint32_t vidpid, const char *name) {
    if (!di || !name || strlen(name) >= sizeof di->pad_name) return GU_STOP_DI_BAD_ARGS;
    di->pad_vidpid = vidpid;
    snprintf(di->pad_name, sizeof di->pad_name, "%s", name);
    return GM_OK;
}
/* Writes a device-instance structure (A or W) into guest memory. */
static int write_device_instance(GuestDinput *di, uint32_t address,
                                 uint32_t type, int wide) {
    uint32_t size = wide ? GDI_DIDEVICEINSTANCE_W_SIZE
                         : GDI_DIDEVICEINSTANCE_A_SIZE;
    uint8_t buffer[GDI_DIDEVICEINSTANCE_W_SIZE];
    memset(buffer, 0, sizeof(buffer));
    put32(buffer + 0, size);
    memcpy(buffer + 4, device_guid(type), 16);
    device_product_guid(di, type, buffer + 20);
    uint32_t devtype = device_devtype(di, type);
    const char *name = type == 0 ? "Keyboard" : (type == 1 ? "Mouse" : "Gamepad");
    if (type == 2 && di->pad_vidpid) {
        /* A HID game pad: DIDEVTYPE_HID | standard-gamepad subtype. */
        devtype = 0x00010000u | (2u << 8) | GDI_DI8DEVTYPE_GAMEPAD;
        name = di->pad_name;
    }
    put32(buffer + 36, devtype);
    uint32_t offset = 40;
    uint32_t unit = wide ? 2u : 1u;
    for (uint32_t copy = 0; copy < 2; copy++) {
        for (uint32_t i = 0; name[i] && i < 259u; i++) {
            buffer[offset + i * unit] = (uint8_t)name[i];
            if (wide) buffer[offset + i * unit + 1] = 0;
        }
        offset += 260u * unit;
    }
    if (type == 2 && di->pad_vidpid) {
        /* guidFFDriver stays zero (no DirectInput force feedback driver);
         * HID usage page 1 (generic desktop), usage 5 (game pad). */
        buffer[offset + 16] = 1;
        buffer[offset + 18] = 5;
    }
    uint32_t total = wide ? GDI_DIDEVICEINSTANCE_W_SIZE
                          : GDI_DIDEVICEINSTANCE_A_SIZE;
    return gm_write(di->memory, address, buffer, total);
}
static int write_device_caps(GuestDinput *di, uint32_t address, uint32_t type) {
    uint8_t caps[GDI_DIDEVCAPS_SIZE];
    memset(caps, 0, sizeof(caps));
    put32(caps + 0, GDI_DIDEVCAPS_SIZE);
    put32(caps + 4, 0x00000001u); /* DIDC_ATTACHED */
    put32(caps + 8, device_devtype(di, type));
    if (type == 2) {
        put32(caps + 12, 6);  /* axes */
        put32(caps + 16, 32); /* buttons */
        put32(caps + 20, 4);  /* POVs */
    } else if (type == 1) {
        put32(caps + 12, 3);
        put32(caps + 16, 4);
    } else {
        put32(caps + 16, 256);
    }
    return gm_write(di->memory, address, caps, sizeof(caps));
}
/* Fills the guest device-state buffer from the host input state. */
static int write_device_state(GuestDinput *di, uint32_t type, uint32_t data_size,
                              uint32_t buffer) {
    uint8_t state[GDI_JOYSTATE2_BYTES];
    memset(state, 0, sizeof(state));
    uint32_t produced = 0;
    if (type == 0) {
        /* c_dfDIKeyboard: one byte per key, the high bit is "down" (dinput.h:
         * "the high-order bit of the byte is set if the key is down"). */
        for (uint32_t i = 0; i < GDI_KEYBOARD_BYTES; i++)
            state[i] = di->host.keys[i] ? 0x80u : 0u;
        produced = GDI_KEYBOARD_BYTES;
    } else if (type == 1) {
        put32(state + 0, (uint32_t)di->host.mouse_x);
        put32(state + 4, (uint32_t)di->host.mouse_y);
        put32(state + 8, (uint32_t)di->host.mouse_z);
        for (uint32_t i = 0; i < 4; i++)
            state[12 + i] = ((di->host.mouse_buttons >> i) & 1u) ? 0x80u : 0u;
        produced = GDI_MOUSE_BYTES;
    } else {
        for (uint32_t i = 0; i < 6; i++)
            put32(state + i * 4u, (uint32_t)di->host.axis[i]);
        for (uint32_t i = 0; i < 4; i++)
            put32(state + 24u + i * 4u, di->host.pov[i]);
        for (uint32_t i = 0; i < 32; i++)
            state[40 + i] = di->host.buttons[i] ? 0x80u : 0u;
        produced = GDI_JOYSTATE_BYTES;
    }
    uint32_t copy = data_size < produced ? data_size : produced;
    if (!copy) return GM_OK;
    return gm_write(di->memory, buffer, state, copy);
}

/* --- IUnknown ----------------------------------------------------------- */
static int query_interface(GuestDinput *di, GdiObject *record, uint32_t object,
                           const uint32_t *args, uint32_t *result) {
    uint8_t riid[16];
    int rc = gm_read(di->memory, args[1], riid, sizeof(riid));
    if (rc) return rc;
    const GdiInterface *iface = &gdi_interfaces[record->interface_index];
    int match = iid_matches(riid, gdi_iunknown) || iid_matches(riid, iface->iid);
    if (!match) {
        rc = write_u32(di, args[2], 0);
        if (rc) return rc;
        *result = GDI_E_NOINTERFACE;
        return GM_OK;
    }
    rc = write_u32(di, args[2], object);
    if (rc) return rc;
    record->refcount++;
    rc = write_u32(di, object + 4, record->refcount);
    if (rc) return rc;
    *result = GDI_S_OK;
    return GM_OK;
}
static int add_ref(GuestDinput *di, GdiObject *record, uint32_t object,
                   uint32_t *result) {
    if (record->refcount == UINT32_MAX) return GU_STOP_DI_BAD_OBJECT;
    record->refcount++;
    int rc = write_u32(di, object + 4, record->refcount);
    if (rc) return rc;
    *result = record->refcount;
    return GM_OK;
}
static int release(GuestDinput *di, GdiObject *record, uint32_t object,
                   uint32_t *result) {
    if (!record->refcount) return GU_STOP_DI_BAD_OBJECT;
    record->refcount--;
    int rc = write_u32(di, object + 4, record->refcount);
    if (rc) return rc;
    if (!record->refcount) {
        record->used = 0;
        if (di->live_objects) di->live_objects--;
    }
    *result = record->refcount;
    return GM_OK;
}

/* --- IDirectInput8 ------------------------------------------------------ */
static int create_device(GuestDinput *di, GdiObject *record,
                         const uint32_t *args, uint32_t *result) {
    uint8_t guid[16];
    int rc = gm_read(di->memory, args[1], guid, sizeof(guid));
    if (rc) return rc;
    if (args[3]) {
        rc = write_u32(di, args[2], 0);
        if (rc) return rc;
        *result = GDI_CLASS_E_NOAGGREGATION;
        return GM_OK;
    }
    uint32_t type;
    uint8_t product[16];
    device_product_guid(di, 2, product);
    if (iid_matches(guid, gdi_sys_keyboard)) type = 0;
    else if (iid_matches(guid, gdi_sys_mouse)) type = 1;
    else if (iid_matches(guid, gdi_pad_instance) || iid_matches(guid, product)) type = 2;
    else type = 3; /* no such device */
    if (type > 2 || !device_present(di, type)) {
        rc = write_u32(di, args[2], 0);
        if (rc) return rc;
        *result = GDI_DIERR_DEVICENOTREG;
        return GM_OK;
    }
    /* The device inherits the A/W flavour of the creating interface. */
    uint32_t interface_index = record->interface_index == 0 ? 2u : 3u;
    uint32_t object = 0;
    rc = create_object(di, interface_index, &object);
    if (rc) return rc;
    GdiObject *device = find_object(di, object);
    if (!device) return GU_STOP_DI_BAD_OBJECT;
    device->device_type = type;
    rc = write_u32(di, args[2], object);
    if (rc) return rc;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int enum_devices(GuestDinput *di, GdiObject *record,
                        const uint32_t *args, uint32_t *result) {
    if (!di->call) return stop(di, GU_STOP_DI_NO_GUEST_CALL,
                               "EnumDevices: no guest-call hook installed");
    uint32_t filter = args[1] & 0xffu;
    uint32_t callback = args[2], reference = args[3];
    int wide = gdi_interfaces[record->interface_index].wide;
    uint32_t count = 0;
    for (uint32_t type = 0; type < 3; type++) {
        if (!device_present(di, type)) continue;
        if (filter != GDI_DI8DEVCLASS_ALL) {
            if (filter == GDI_DI8DEVCLASS_KEYBOARD && type != 0) continue;
            if (filter == GDI_DI8DEVCLASS_POINTER && type != 1) continue;
            if (filter == GDI_DI8DEVCLASS_GAMECTRL && type != 2) continue;
        }
        /* The instance structure is written to a scratch slot inside the
         * arena so the guest callback can read it. */
        uint32_t scratch = di->arena_base + di->arena_size;
        int rc = write_device_instance(di, scratch, type, wide);
        if (rc) return rc;
        uint32_t call_args[2];
        call_args[0] = scratch;
        call_args[1] = reference;
        uint32_t answer = 0;
        rc = di->call(di->call_context, callback, call_args, 2, &answer);
        if (rc) return rc;
        count++;
        if (answer == GDI_DIENUM_STOP) break;
    }
    *result = GDI_DI_OK;
    (void)count;
    return GM_OK;
}
/* --- IDirectInputDevice8::EnumObjects (keyboard) ------------------------ */
/* GUID_Key {55728220-D33C-11CF-BFC7-444553540000} */
static const uint8_t gdi_guid_key[16] = GDI_IID(0x55728220u, 0xd33cu, 0x11cfu,
    0xbf, 0xc7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
/* Key names as the US layout reports them (GetKeyNameText). */
static const char *key_name(uint32_t dik, char *buffer) {
    static const struct { uint8_t dik; const char *name; } names[] = {
        {0x01,"Esc"},{0x0c,"-"},{0x0d,"="},{0x0e,"Backspace"},{0x0f,"Tab"},{0x1a,"["},{0x1b,"]"},
        {0x1c,"Enter"},{0x1d,"Ctrl"},{0x27,";"},{0x28,"'"},{0x29,"`"},{0x2a,"Shift"},{0x2b,"\\"},
        {0x33,","},{0x34,"."},{0x35,"/"},{0x36,"Right Shift"},{0x37,"Num *"},{0x38,"Alt"},
        {0x39,"Space"},{0x3a,"Caps Lock"},{0x45,"Num Lock"},{0x46,"Scroll Lock"},{0x47,"Num 7"},
        {0x48,"Num 8"},{0x49,"Num 9"},{0x4a,"Num -"},{0x4b,"Num 4"},{0x4c,"Num 5"},{0x4d,"Num 6"},
        {0x4e,"Num +"},{0x4f,"Num 1"},{0x50,"Num 2"},{0x51,"Num 3"},{0x52,"Num 0"},{0x53,"Num Del"},
        {0x57,"F11"},{0x58,"F12"},{0x9c,"Num Enter"},{0x9d,"Right Ctrl"},{0xb5,"Num /"},
        {0xb7,"Prnt Scrn"},{0xb8,"Right Alt"},{0xc7,"Home"},{0xc8,"Up"},{0xc9,"Page Up"},
        {0xcb,"Left"},{0xcd,"Right"},{0xcf,"End"},{0xd0,"Down"},{0xd1,"Page Down"},{0xd2,"Insert"},
        {0xd3,"Delete"},{0xdb,"Left Windows"},{0xdc,"Right Windows"},{0xdd,"Application"} };
    static const char row1[] = "1234567890", row2[] = "QWERTYUIOP", row3[] = "ASDFGHJKL", row4[] = "ZXCVBNM";
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (names[i].dik == dik) return names[i].name;
    buffer[1] = 0;
    if (dik >= 0x02 && dik <= 0x0b) { buffer[0] = row1[dik - 0x02]; return buffer; }
    if (dik >= 0x10 && dik <= 0x19) { buffer[0] = row2[dik - 0x10]; return buffer; }
    if (dik >= 0x1e && dik <= 0x26) { buffer[0] = row3[dik - 0x1e]; return buffer; }
    if (dik >= 0x2c && dik <= 0x32) { buffer[0] = row4[dik - 0x2c]; return buffer; }
    if (dik >= 0x3b && dik <= 0x44) { snprintf(buffer, 8, "F%u", dik - 0x3a); return buffer; }
    snprintf(buffer, 16, "Key %02X", dik);
    return buffer;
}
static int device_enum_objects(GuestDinput *di, GdiObject *record,
                               const uint32_t *args, uint32_t *result) {
    /* (this, lpCallback, pvRef, dwFlags) */
    if (record->device_type != 0)
        return stop(di, GU_STOP_DI_UNIMPLEMENTED, "EnumObjects: only the keyboard is modelled");
    if (!di->call) return stop(di, GU_STOP_DI_NO_GUEST_CALL,
                               "EnumObjects: no guest-call hook installed");
    if (!args[1]) { *result = GDI_DIERR_INVALIDPARAM; return GM_OK; }
    uint32_t flags = args[3];
    /* DIDFT_ALL (0) or any button type selects the keys; axes/POVs: none. */
    if (flags && !(flags & 0x0000000cu)) { *result = GDI_DI_OK; return GM_OK; }
    int wide = gdi_interfaces[record->interface_index].wide;
    uint32_t size = wide ? 576u : 316u;
    uint32_t scratch = di->arena_base + di->arena_size;
    for (uint32_t dik = 0; dik < 256; dik++) {
        if (!di->key_present[dik]) continue;
        uint8_t block[576];
        memset(block, 0, sizeof block);
        put32(block + 0, size);
        memcpy(block + 4, gdi_guid_key, 16);
        put32(block + 20, dik);                       /* dwOfs: c_dfDIKeyboard offset */
        put32(block + 24, (dik << 8) | 0x00000004u);  /* DIDFT_PSHBUTTON | instance */
        put32(block + 28, 0);                         /* dwFlags */
        char scratch_name[16];
        const char *name = key_name(dik, scratch_name);
        for (uint32_t i = 0; name[i] && i < 259; i++) {
            if (wide) block[32 + i * 2] = (uint8_t)name[i];
            else block[32 + i] = (uint8_t)name[i];
        }
        int rc = gm_write(di->memory, scratch, block, size);
        if (rc) return rc;
        uint32_t call_args[2] = { scratch, args[2] }, answer = 0;
        rc = di->call(di->call_context, args[1], call_args, 2, &answer);
        if (rc) return rc;
        if (answer == GDI_DIENUM_STOP) break;
    }
    *result = GDI_DI_OK;
    return GM_OK;
}
/* IDirectInputDevice8::GetObjectInfo(pdidoi, dwObj, dwHow) for a keyboard key. */
static int device_get_object_info(GuestDinput *di, GdiObject *record,
                                  const uint32_t *args, uint32_t *result) {
    if (record->device_type != 0)
        return stop(di, GU_STOP_DI_UNIMPLEMENTED, "GetObjectInfo: only the keyboard is modelled");
    int wide = gdi_interfaces[record->interface_index].wide;
    uint32_t size = wide ? 576u : 316u;
    uint8_t head[4];
    if (!args[1]) { *result = GDI_DIERR_INVALIDPARAM; return GM_OK; }
    int rc = gm_read(di->memory, args[1], head, 4);
    if (rc) return rc;
    uint32_t given = (uint32_t)head[0] | ((uint32_t)head[1] << 8) | ((uint32_t)head[2] << 16) | ((uint32_t)head[3] << 24);
    if (given != size) { *result = GDI_DIERR_INVALIDPARAM; return GM_OK; }
    uint32_t dik;
    if (args[3] == 1u) dik = args[2];                         /* DIPH_BYOFFSET */
    else if (args[3] == 2u) dik = (args[2] >> 8) & 0xffffu;   /* DIPH_BYID: instance number */
    else { *result = GDI_DIERR_INVALIDPARAM; return GM_OK; }
    if (dik >= 256u || !di->key_present[dik]) { *result = 0x80070002u; return GM_OK; }   /* DIERR_OBJECTNOTFOUND */
    uint8_t block[576];
    memset(block, 0, sizeof block);
    put32(block + 0, size);
    memcpy(block + 4, gdi_guid_key, 16);
    put32(block + 20, dik);
    put32(block + 24, (dik << 8) | 0x00000004u);
    char scratch_name[16];
    const char *name = key_name(dik, scratch_name);
    for (uint32_t i = 0; name[i] && i < 259; i++) {
        if (wide) block[32 + i * 2] = (uint8_t)name[i];
        else block[32 + i] = (uint8_t)name[i];
    }
    rc = gm_write(di->memory, args[1], block, size);
    if (rc) return rc;
    *result = GDI_DI_OK;
    return GM_OK;
}
void gdi_set_keyboard_keys(GuestDinput *di, const uint8_t *present) {
    if (di && present) memcpy(di->key_present, present, 256);
}

static int get_device_status(GuestDinput *di, const uint32_t *args,
                             uint32_t *result) {
    uint8_t guid[16];
    int rc = gm_read(di->memory, args[1], guid, sizeof(guid));
    if (rc) return rc;
    uint32_t type = 2;
    if (iid_matches(guid, gdi_sys_keyboard)) type = 0;
    else if (iid_matches(guid, gdi_sys_mouse)) type = 1;
    *result = device_present(di, type) ? GDI_DI_OK : GDI_DI_NOTATTACHED;
    return GM_OK;
}

/* --- IDirectInputDevice8 ------------------------------------------------ */
static int device_set_data_format(GuestDinput *di, GdiObject *record,
                                  const uint32_t *args, uint32_t *result) {
    uint8_t format[GDI_DIDATAFORMAT_SIZE];
    int rc = gm_read(di->memory, args[1], format, sizeof(format));
    if (rc) return rc;
    uint32_t size = get32(format + 0);
    if (size < 24u || size > GDI_DIDATAFORMAT_SIZE)
        return stop(di, GU_STOP_DI_FORMAT,
                    "SetDataFormat: DIDATAFORMAT.dwSize not modelled");
    uint32_t data_size = get32(format + 12);
    switch (data_size) {
    case GDI_KEYBOARD_BYTES:
        if (record->device_type != 0)
            return stop(di, GU_STOP_DI_FORMAT,
                        "SetDataFormat: 256-byte format on a non-keyboard");
        break;
    case GDI_MOUSE_BYTES:
        if (record->device_type != 1)
            return stop(di, GU_STOP_DI_FORMAT,
                        "SetDataFormat: 16-byte format on a non-mouse");
        break;
    case GDI_JOYSTATE_BYTES:
    case GDI_JOYSTATE2_BYTES:
        if (record->device_type != 2)
            return stop(di, GU_STOP_DI_FORMAT,
                        "SetDataFormat: joystick format on a non-joystick");
        break;
    default:
        return stop(di, GU_STOP_DI_FORMAT,
                    "SetDataFormat: dwDataSize is not c_dfDI*");
    }
    record->data_size = data_size;
    record->has_format = 1;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_set_cooperative_level(GuestDinput *di, GdiObject *record,
                                        const uint32_t *args,
                                        uint32_t *result) {
    uint32_t flags = args[2];
    if (flags & (GDI_DISCL_EXCLUSIVE | GDI_DISCL_BACKGROUND))
        return stop(di, GU_STOP_DI_BAD_ARGS,
                    "SetCooperativeLevel: only non-exclusive foreground is "
                    "modelled");
    if (!(flags & GDI_DISCL_NONEXCLUSIVE))
        return stop(di, GU_STOP_DI_BAD_ARGS,
                    "SetCooperativeLevel: DISCL_NONEXCLUSIVE is required");
    record->cooperative = flags;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_acquire(GuestDinput *di, GdiObject *record,
                          uint32_t *result) {
    (void)di;
    if (!record->has_format || !record->cooperative) {
        *result = GDI_DIERR_NOTINITIALIZED;
        return GM_OK;
    }
    record->acquired = 1;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_get_state(GuestDinput *di, GdiObject *record,
                            const uint32_t *args, uint32_t *result) {
    if (!record->acquired) {
        *result = GDI_DIERR_NOTACQUIRED;
        return GM_OK;
    }
    uint32_t capacity = args[1];
    if (!capacity || !args[2]) {
        *result = GDI_DIERR_INVALIDPARAM;
        return GM_OK;
    }
    if (capacity < record->data_size) {
        *result = GDI_DIERR_INVALIDPARAM;
        return GM_OK;
    }
    int rc = write_device_state(di, record->device_type, record->data_size,
                                args[2]);
    if (rc) return rc;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_poll(GuestDinput *di, GdiObject *record, uint32_t *result) {
    (void)di;
    if (!record->acquired) {
        *result = GDI_DIERR_NOTACQUIRED;
        return GM_OK;
    }
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_get_capabilities(GuestDinput *di, GdiObject *record,
                                   const uint32_t *args, uint32_t *result) {
    uint8_t size_bytes[4];
    int rc = gm_read(di->memory, args[1], size_bytes, 4);
    if (rc) return rc;
    if (get32(size_bytes) < GDI_DIDEVCAPS_SIZE) {
        *result = GDI_DIERR_INVALIDPARAM;
        return GM_OK;
    }
    rc = write_device_caps(di, args[1], record->device_type);
    if (rc) return rc;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_get_info(GuestDinput *di, GdiObject *record,
                           const uint32_t *args, uint32_t *result) {
    uint8_t size_bytes[4];
    int rc = gm_read(di->memory, args[1], size_bytes, 4);
    if (rc) return rc;
    int wide = gdi_interfaces[record->interface_index].wide;
    uint32_t needed = wide ? GDI_DIDEVICEINSTANCE_W_SIZE
                           : GDI_DIDEVICEINSTANCE_A_SIZE;
    if (get32(size_bytes) < needed) {
        *result = GDI_DIERR_INVALIDPARAM;
        return GM_OK;
    }
    rc = write_device_instance(di, args[1], record->device_type, wide);
    if (rc) return rc;
    *result = GDI_DI_OK;
    return GM_OK;
}
static int device_property(GuestDinput *di, GdiObject *record,
                           const uint32_t *args, uint32_t *result, int set) {
    /* rguidProp is either a GUID pointer or MAKEDIPROP(n), a small integer
     * (dinput.h). Only the two identity properties of the game controller are
     * modelled; everything else is the documented DIERR_UNSUPPORTED. */
    uint32_t property = args[1];
    if (property < 0x10000u) {
        if (!set && record->device_type == 2 && (property == 15u || property == 24u)) {
            /* DIPROP_JOYSTICKID (15) / DIPROP_VIDPID (24): DIPROPDWORD. */
            uint8_t header[16];
            int rc = gm_read(di->memory, args[2], header, sizeof(header));
            if (rc) return rc;
            if (get32(header + 0) != 20u || get32(header + 4) != 16u) {
                *result = GDI_DIERR_INVALIDPARAM;
                return GM_OK;
            }
            if (property == 24u && !di->pad_vidpid) {
                *result = GDI_DIERR_UNSUPPORTED;
                return GM_OK;
            }
            rc = write_u32(di, args[2] + 16u, property == 15u ? 0u : di->pad_vidpid);
            if (rc) return rc;
            *result = GDI_DI_OK;
            return GM_OK;
        }
        *result = GDI_DIERR_UNSUPPORTED;
        return GM_OK;
    }
    uint8_t guid[16];
    int rc = gm_read(di->memory, property, guid, sizeof(guid));
    if (rc) return rc;
    if (iid_matches(guid, gdi_prop_buffersize)) {
        /* Buffered data is not modelled, so the size is accepted and ignored. */
        *result = set ? GDI_DI_OK : GDI_DIERR_UNSUPPORTED;
        return GM_OK;
    }
    *result = GDI_DIERR_UNSUPPORTED;
    return GM_OK;
}

/* --- dispatch ----------------------------------------------------------- */
int gdi_call(GuestDinput *di, uint32_t trap_address, const uint32_t *args,
             uint32_t *result) {
    if (!di || !args || !result) return GU_STOP_DI_BAD_ARGS;
    uint32_t interface_index = 0, slot = 0;
    int rc = gdi_resolve(di, trap_address, &interface_index, &slot);
    if (rc) return rc;
    GdiObject *record = find_object(di, args[0]);
    if (!record) return GU_STOP_DI_BAD_OBJECT;
    if (record->interface_index != interface_index)
        return GU_STOP_DI_BAD_OBJECT;
    const GdiInterface *iface = &gdi_interfaces[interface_index];
    switch (slot) {
    case 0: return query_interface(di, record, args[0], args, result);
    case 1: return add_ref(di, record, args[0], result);
    case 2: return release(di, record, args[0], result);
    default: break;
    }
    if (!iface->device) {
        switch (slot) {
        case 3: return create_device(di, record, args, result);
        case 4: return enum_devices(di, record, args, result);
        case 5: return get_device_status(di, args, result);
        default: break;
        }
    } else {
        switch (slot) {
        case 3: return device_get_capabilities(di, record, args, result);
        case 4: return device_enum_objects(di, record, args, result);
        case 5: return device_property(di, record, args, result, 0);
        case 6: return device_property(di, record, args, result, 1);
        case 7: return device_acquire(di, record, result);
        case 8: record->acquired = 0; *result = GDI_DI_OK; return GM_OK;
        case 9: return device_get_state(di, record, args, result);
        case 11: return device_set_data_format(di, record, args, result);
        case 13: return device_set_cooperative_level(di, record, args, result);
        case 14: return device_get_object_info(di, record, args, result);
        case 15: return device_get_info(di, record, args, result);
        case 25: return device_poll(di, record, result);
        default: break;
        }
    }
    snprintf(di->last_stop, sizeof(di->last_stop), "%s::%s", iface->name,
             iface->methods[slot].name);
    fprintf(stderr, "dinput-unimplemented: %s (slot %u, %u arg bytes)\n",
            di->last_stop, slot, iface->methods[slot].arg_bytes);
    return GU_STOP_DI_UNIMPLEMENTED;
}

/* --- DirectInput8Create ------------------------------------------------- */
int gdi_direct_input8_create(GuestDinput *di, const uint32_t *args,
                             uint32_t *result) {
    if (!di || !args || !result) return GU_STOP_DI_BAD_ARGS;
    if (args[1] != GDI_DIRECTINPUT_VERSION) {
        *result = GDI_DIERR_OLDDIRECTINPUTVERSION;
        return GM_OK;
    }
    uint8_t riid[16];
    int rc = gm_read(di->memory, args[2], riid, sizeof(riid));
    if (rc) return rc;
    uint32_t interface_index = UINT32_MAX;
    for (uint32_t i = 0; i < 2; i++)
        if (iid_matches(riid, gdi_interfaces[i].iid)) interface_index = i;
    if (interface_index == UINT32_MAX) {
        *result = GDI_E_NOINTERFACE;
        return GM_OK;
    }
    if (args[4]) {
        rc = write_u32(di, args[3], 0);
        if (rc) return rc;
        *result = GDI_CLASS_E_NOAGGREGATION;
        return GM_OK;
    }
    uint32_t object = 0;
    rc = create_object(di, interface_index, &object);
    if (rc) return rc;
    rc = write_u32(di, args[3], object);
    if (rc) return rc;
    *result = GDI_DI_OK;
    return GM_OK;
}

/* --- registration list -------------------------------------------------- */
const GuestDinputExport dinput8_exports[] = {
    { "dinput8.dll", "DirectInput8Create", 20, gdi_direct_input8_create }
};
const uint32_t dinput8_export_count =
    (uint32_t)(sizeof(dinput8_exports) / sizeof(dinput8_exports[0]));
