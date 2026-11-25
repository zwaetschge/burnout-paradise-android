/* Generic 32-bit COM skeleton: guest objects, guest vtables, trap resolver.
 *
 * Only IUnknown is executed. Everything else is a named stop that prints
 *   d3d11-unimplemented: <Interface>::<Method>
 * and returns GU_STOP_COM_UNIMPLEMENTED, so an unimplemented method can never
 * look like a successful HRESULT. Guest memory is only touched through
 * gm_check/gm_read/gm_write; no host pointer is stored in guest memory. */
#include "guest_com.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCOM_MAX_OBJECTS 256u
#define GCOM_MAX_FEATURE_LEVELS 32u
#define D3D11_SDK_VERSION_ 7u
#define DXGI_SWAP_CHAIN_DESC_SIZE 60u

typedef struct {
    int used;
    uint32_t object;
    uint32_t interface_index;
    uint32_t refcount;
} GuComObject;

struct GuestCom {
    GuestMemory *memory;
    uint32_t teb;
    uint32_t arena_base, arena_size, arena_used;
    GuComObject objects[GCOM_MAX_OBJECTS];
    uint32_t live_objects;
    char last_stop[128];
    char last_interface[64];
    uint32_t last_stop_slot;
};

static void put32(uint8_t *b, uint32_t value) {
    b[0] = (uint8_t)value; b[1] = (uint8_t)(value >> 8);
    b[2] = (uint8_t)(value >> 16); b[3] = (uint8_t)(value >> 24);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
static int write_u32(GuestCom *com, uint32_t address, uint32_t value) {
    uint8_t b[4]; put32(b, value); return gm_write(com->memory, address, b, 4);
}
static int last_error(GuestCom *com, uint32_t value) {
    return write_u32(com, com->teb + 0x34, value);
}

GuestCom *gcom_create(GuestMemory *memory, uint32_t teb) {
    if (!memory) return NULL;
    GuestCom *com = (GuestCom *)calloc(1, sizeof(*com));
    if (!com) return NULL;
    com->memory = memory;
    com->teb = teb;
    return com;
}
void gcom_destroy(GuestCom *com) { free(com); }

int gcom_set_arena(GuestCom *com, uint32_t base, uint32_t size) {
    if (!com || !size) return GM_UNSUPPORTED;
    int rc = gm_check(com->memory, base, size, GM_WRITE);
    if (rc) return rc;
    com->arena_base = base;
    com->arena_size = size;
    com->arena_used = 0;
    return GM_OK;
}

/* --- interface table ---------------------------------------------------- */
uint32_t gcom_interface_count(void) { return gu_com_interface_count; }
const GuComInterface *gcom_interface_at(uint32_t index) {
    if (index >= gu_com_interface_count) return NULL;
    return &gu_com_interfaces[index];
}
int gcom_interface_index(const char *name) {
    if (!name) return -1;
    for (uint32_t i = 0; i < gu_com_interface_count; i++)
        if (strcmp(gu_com_interfaces[i].name, name) == 0) return (int)i;
    return -1;
}
uint32_t gcom_total_methods(void) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < gu_com_interface_count; i++)
        total += gu_com_interfaces[i].method_count;
    return total;
}
uint32_t gcom_method_count(uint32_t interface_index) {
    if (interface_index >= gu_com_interface_count) return 0;
    return gu_com_interfaces[interface_index].method_count;
}
uint32_t gcom_method_arg_bytes(uint32_t interface_index, uint32_t slot) {
    if (interface_index >= gu_com_interface_count) return 0;
    const GuComInterface *iface = &gu_com_interfaces[interface_index];
    if (slot >= iface->method_count) return 0;
    return iface->methods[slot].arg_bytes;
}
/* Global slot index of (interface, slot); 0xffffffff when out of range. */
static uint32_t global_index(uint32_t interface_index, uint32_t slot) {
    if (interface_index >= gu_com_interface_count) return UINT32_MAX;
    const GuComInterface *iface = &gu_com_interfaces[interface_index];
    if (slot >= iface->method_count) return UINT32_MAX;
    uint32_t base = 0;
    for (uint32_t i = 0; i < interface_index; i++)
        base += gu_com_interfaces[i].method_count;
    return base + slot;
}
uint32_t gcom_trap_address(uint32_t interface_index, uint32_t slot) {
    uint32_t index = global_index(interface_index, slot);
    if (index == UINT32_MAX) return 0;
    return GCOM_TRAP_BASE + index * GCOM_TRAP_STRIDE;
}
int gcom_resolve(GuestCom *com, uint32_t address, uint32_t *interface_index,
                 uint32_t *slot) {
    (void)com;
    if (!interface_index || !slot) return GU_STOP_COM_BAD_ARGS;
    if (address < GCOM_TRAP_BASE || address >= GCOM_TRAP_LIMIT)
        return GU_STOP_COM_BAD_OBJECT;
    uint32_t delta = address - GCOM_TRAP_BASE;
    if (delta % GCOM_TRAP_STRIDE) return GU_STOP_COM_BAD_OBJECT;
    uint32_t index = delta / GCOM_TRAP_STRIDE;
    for (uint32_t i = 0; i < gu_com_interface_count; i++) {
        uint32_t count = gu_com_interfaces[i].method_count;
        if (index < count) { *interface_index = i; *slot = index; return 0; }
        index -= count;
    }
    return GU_STOP_COM_BAD_OBJECT;
}

/* --- guest objects ------------------------------------------------------ */
static GuComObject *find_object(GuestCom *com, uint32_t object) {
    for (uint32_t i = 0; i < GCOM_MAX_OBJECTS; i++)
        if (com->objects[i].used && com->objects[i].object == object)
            return &com->objects[i];
    return NULL;
}
uint32_t gcom_live_objects(GuestCom *com) { return com ? com->live_objects : 0u; }
uint32_t gcom_refcount(GuestCom *com, uint32_t object) {
    GuComObject *record = find_object(com, object);
    return record ? record->refcount : 0u;
}
int gcom_object_vtable(GuestCom *com, uint32_t object, uint32_t *vtable) {
    if (!com || !vtable) return GU_STOP_COM_BAD_ARGS;
    if (!find_object(com, object)) return GU_STOP_COM_BAD_OBJECT;
    uint8_t bytes[4];
    int rc = gm_read(com->memory, object, bytes, 4);
    if (rc) return rc;
    *vtable = get32(bytes);
    return 0;
}
int gcom_create_object(GuestCom *com, uint32_t interface_index, uint32_t *object) {
    if (!com || !object) return GU_STOP_COM_BAD_ARGS;
    if (interface_index >= gu_com_interface_count) return GU_STOP_COM_BAD_ARGS;
    if (!com->arena_size) return GU_STOP_COM_NO_REGION;
    GuComObject *record = NULL;
    for (uint32_t i = 0; i < GCOM_MAX_OBJECTS; i++)
        if (!com->objects[i].used) { record = &com->objects[i]; break; }
    if (!record) return GU_STOP_COM_REGION_FULL;
    const GuComInterface *iface = &gu_com_interfaces[interface_index];
    uint32_t vtable_size = iface->method_count * 4u;
    uint32_t needed = ((vtable_size + 15u) & ~15u) + GCOM_OBJECT_SIZE;
    if (com->arena_used + needed > com->arena_size) return GU_STOP_COM_REGION_FULL;
    uint32_t vtable = com->arena_base + com->arena_used;
    uint32_t self = vtable + ((vtable_size + 15u) & ~15u);
    com->arena_used += needed;

    uint8_t entries[4];
    for (uint32_t slot = 0; slot < iface->method_count; slot++) {
        uint32_t trap = gcom_trap_address(interface_index, slot);
        put32(entries, trap);
        int rc = gm_write(com->memory, vtable + slot * 4u, entries, 4);
        if (rc) return rc;
    }
    uint8_t block[GCOM_OBJECT_SIZE];
    put32(block + 0, vtable);
    put32(block + 4, 1);
    put32(block + 8, interface_index);
    put32(block + 12, GCOM_MAGIC);
    int rc = gm_write(com->memory, self, block, sizeof(block));
    if (rc) return rc;
    record->used = 1;
    record->object = self;
    record->interface_index = interface_index;
    record->refcount = 1;
    com->live_objects++;
    *object = self;
    return 0;
}

/* --- IUnknown ----------------------------------------------------------- */
static int iid_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 16) == 0;
}
static int query_interface(GuestCom *com, GuComObject *record, uint32_t object,
                           const uint32_t *args, uint32_t *result) {
    uint8_t riid[16];
    int rc = gm_read(com->memory, args[1], riid, sizeof(riid));
    if (rc) return rc;
    const GuComInterface *iface = &gu_com_interfaces[record->interface_index];
    int match = 0;
    if (iid_equal(riid, gu_com_interfaces[0].iid)) match = 1; /* IUnknown */
    else if (iface->iid_known && iid_equal(riid, iface->iid)) match = 1;
    if (!match) {
        rc = write_u32(com, args[2], 0);
        if (rc) return rc;
        *result = GCOM_E_NOINTERFACE;
        return 0;
    }
    rc = write_u32(com, args[2], object);
    if (rc) return rc;
    record->refcount++;
    rc = write_u32(com, object + 4, record->refcount);
    if (rc) return rc;
    *result = GCOM_S_OK;
    return 0;
}
static int add_ref(GuestCom *com, GuComObject *record, uint32_t object,
                   uint32_t *result) {
    if (record->refcount == UINT32_MAX) return GU_STOP_COM_BAD_OBJECT;
    record->refcount++;
    int rc = write_u32(com, object + 4, record->refcount);
    if (rc) return rc;
    *result = record->refcount;
    return 0;
}
static int release(GuestCom *com, GuComObject *record, uint32_t object,
                   uint32_t *result) {
    if (!record->refcount) return GU_STOP_COM_BAD_OBJECT;
    record->refcount--;
    int rc = write_u32(com, object + 4, record->refcount);
    if (rc) return rc;
    if (!record->refcount) {
        record->used = 0;
        if (com->live_objects) com->live_objects--;
    }
    *result = record->refcount;
    return 0;
}

/* --- dispatch ----------------------------------------------------------- */
const char *gcom_last_stop(GuestCom *com) {
    return com && com->last_stop[0] ? com->last_stop : NULL;
}
const char *gcom_stop_interface(GuestCom *com) {
    return com && com->last_interface[0] ? com->last_interface : NULL;
}
uint32_t gcom_last_stop_slot(GuestCom *com) { return com ? com->last_stop_slot : 0u; }

int gcom_call(GuestCom *com, uint32_t object, uint32_t slot,
              const uint32_t *args, uint32_t *result) {
    if (!com || !args || !result) return GU_STOP_COM_BAD_ARGS;
    GuComObject *record = find_object(com, object);
    if (!record) return GU_STOP_COM_BAD_OBJECT;
    const GuComInterface *iface = &gu_com_interfaces[record->interface_index];
    if (slot >= iface->method_count) return GU_STOP_COM_BAD_ARGS;
    if (args[0] != object) return GU_STOP_COM_BAD_OBJECT;
    switch (slot) {
    case 0: return query_interface(com, record, object, args, result);
    case 1: return add_ref(com, record, object, result);
    case 2: return release(com, record, object, result);
    default: break;
    }
    const GuComMethod *method = &iface->methods[slot];
    snprintf(com->last_stop, sizeof(com->last_stop), "%s::%s", iface->name,
             method->name);
    snprintf(com->last_interface, sizeof(com->last_interface), "%s", iface->name);
    com->last_stop_slot = slot;
    fprintf(stderr, "d3d11-unimplemented: %s (slot %u, %u arg bytes)\n",
            com->last_stop, slot, method->arg_bytes);
    return GU_STOP_COM_UNIMPLEMENTED;
}
int gcom_call_trap(GuestCom *com, uint32_t trap_address, const uint32_t *args,
                   uint32_t *result) {
    uint32_t interface_index = 0, slot = 0;
    int rc = gcom_resolve(com, trap_address, &interface_index, &slot);
    if (rc) return rc;
    if (!args) return GU_STOP_COM_BAD_ARGS;
    return gcom_call(com, args[0], slot, args, result);
}

/* --- D3D11 / DXGI entry points ----------------------------------------- */
/* Reads the D3D_FEATURE_LEVEL array (4-byte entries) and records it in the
 * stop message; no device is created. */
static int read_feature_levels(GuestCom *com, uint32_t address, uint32_t count,
                               uint32_t *levels, uint32_t *level_count) {
    if (!levels || !level_count) return GU_STOP_COM_BAD_ARGS;
    *level_count = 0;
    if (!address || !count) return 0;
    if (count > GCOM_MAX_FEATURE_LEVELS) return GU_STOP_COM_BAD_ARGS;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t bytes[4];
        int rc = gm_read(com->memory, address + i * 4u, bytes, 4);
        if (rc) return rc;
        levels[i] = get32(bytes);
    }
    *level_count = count;
    return 0;
}
static int stop_entry(GuestCom *com, const char *what, uint32_t detail,
                      uint32_t *result) {
    snprintf(com->last_stop, sizeof(com->last_stop), "%s", what);
    snprintf(com->last_interface, sizeof(com->last_interface), "%s", what);
    com->last_stop_slot = detail;
    fprintf(stderr, "d3d11-entry: %s (detail %#x) -- no device backend\n",
            what, detail);
    *result = 0;
    return GU_STOP_COM_ENTRY;
}
int gcom_d3d11_create_device(GuestCom *com, const uint32_t *args,
                             uint32_t *result) {
    if (!com || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t levels[GCOM_MAX_FEATURE_LEVELS], count = 0;
    int rc = read_feature_levels(com, args[4], args[5], levels, &count);
    if (rc) return rc;
    if (args[6] != D3D11_SDK_VERSION_) {
        rc = last_error(com, 87); if (rc) return rc;
        *result = GCOM_E_INVALIDARG;
        return 0;
    }
    if (count) {
        fprintf(stderr, "d3d11-entry: D3D11CreateDevice feature levels:");
        for (uint32_t i = 0; i < count; i++) fprintf(stderr, " %#x", levels[i]);
        fprintf(stderr, "\n");
    }
    return stop_entry(com, "D3D11CreateDevice", count ? levels[0] : 0, result);
}
int gcom_d3d11_create_device_and_swap_chain(GuestCom *com, const uint32_t *args,
                                            uint32_t *result) {
    if (!com || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint32_t levels[GCOM_MAX_FEATURE_LEVELS], count = 0;
    int rc = read_feature_levels(com, args[4], args[5], levels, &count);
    if (rc) return rc;
    if (args[6] != D3D11_SDK_VERSION_) {
        rc = last_error(com, 87); if (rc) return rc;
        *result = GCOM_E_INVALIDARG;
        return 0;
    }
    uint8_t desc[DXGI_SWAP_CHAIN_DESC_SIZE];
    if (args[7]) {
        rc = gm_read(com->memory, args[7], desc, sizeof(desc));
        if (rc) return rc;
        fprintf(stderr,
                "d3d11-entry: swap chain desc %ux%u fmt=%u count=%u windowed=%u "
                "swapEffect=%u flags=%#x hwnd=%#x\n",
                get32(desc + 0), get32(desc + 4), get32(desc + 16),
                get32(desc + 40), get32(desc + 48), get32(desc + 52),
                get32(desc + 56), get32(desc + 44));
    }
    return stop_entry(com, "D3D11CreateDeviceAndSwapChain",
                      count ? levels[0] : 0, result);
}
static int create_factory(GuestCom *com, const uint32_t *args, uint32_t *result,
                          const char *what, uint32_t expected) {
    if (!com || !args || !result) return GU_STOP_COM_BAD_ARGS;
    uint8_t riid[16];
    int rc = gm_read(com->memory, args[0], riid, sizeof(riid));
    if (rc) return rc;
    int known = iid_equal(riid, gu_com_interfaces[0].iid) ||
                iid_equal(riid, gu_com_interfaces[expected].iid);
    if (!known) {
        rc = write_u32(com, args[1], 0);
        if (rc) return rc;
        *result = GCOM_E_NOINTERFACE;
        return 0;
    }
    return stop_entry(com, what, expected, result);
}
int gcom_create_dxgi_factory(GuestCom *com, const uint32_t *args,
                             uint32_t *result) {
    int index = gcom_interface_index("IDXGIFactory");
    if (index < 0) return GU_STOP_COM_BAD_ARGS;
    return create_factory(com, args, result, "CreateDXGIFactory", (uint32_t)index);
}
int gcom_create_dxgi_factory1(GuestCom *com, const uint32_t *args,
                              uint32_t *result) {
    int index = gcom_interface_index("IDXGIFactory1");
    if (index < 0) return GU_STOP_COM_BAD_ARGS;
    return create_factory(com, args, result, "CreateDXGIFactory1", (uint32_t)index);
}

/* --- registration list -------------------------------------------------- */
const GuComExport gu_com_exports[] = {
    { "d3d11.dll", "D3D11CreateDevice", 40, gcom_d3d11_create_device },
    { "d3d11.dll", "D3D11CreateDeviceAndSwapChain", 48,
      gcom_d3d11_create_device_and_swap_chain },
    { "dxgi.dll", "CreateDXGIFactory", 8, gcom_create_dxgi_factory },
    { "dxgi.dll", "CreateDXGIFactory1", 8, gcom_create_dxgi_factory1 }
};
const uint32_t gu_com_export_count =
    (uint32_t)(sizeof(gu_com_exports) / sizeof(gu_com_exports[0]));
