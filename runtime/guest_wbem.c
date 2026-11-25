/* WMI guest COM objects. See guest_wbem.h for the modelled scope. */
#include "guest_wbem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { GWB_LOCATOR = 0, GWB_SERVICES, GWB_ENUM, GWB_OBJECT, GWB_KIND_COUNT };

typedef struct { const char *name; uint32_t args; } GwbMethod;   /* args without `this` */

static const GwbMethod locator_methods[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"ConnectServer", 8}
};
static const GwbMethod services_methods[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"OpenNamespace", 5}, {"CancelAsyncCall", 1},
    {"QueryObjectSink", 2}, {"GetObject", 5}, {"GetObjectAsync", 4}, {"PutClass", 4}, {"PutClassAsync", 4},
    {"DeleteClass", 4}, {"DeleteClassAsync", 4}, {"CreateClassEnum", 4}, {"CreateClassEnumAsync", 4},
    {"PutInstance", 4}, {"PutInstanceAsync", 4}, {"DeleteInstance", 4}, {"DeleteInstanceAsync", 4},
    {"CreateInstanceEnum", 4}, {"CreateInstanceEnumAsync", 4}, {"ExecQuery", 5}, {"ExecQueryAsync", 5},
    {"ExecNotificationQuery", 5}, {"ExecNotificationQueryAsync", 5}, {"ExecMethod", 7}, {"ExecMethodAsync", 6}
};
static const GwbMethod enum_methods[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"Reset", 0}, {"Next", 4}, {"NextAsync", 2},
    {"Clone", 1}, {"Skip", 2}
};
static const GwbMethod object_methods[] = {
    {"QueryInterface", 2}, {"AddRef", 0}, {"Release", 0}, {"GetQualifierSet", 1}, {"Get", 5}, {"Put", 4},
    {"Delete", 1}, {"GetNames", 5}, {"BeginEnumeration", 1}, {"Next", 5}, {"EndEnumeration", 0},
    {"GetPropertyQualifierSet", 2}, {"Clone", 1}, {"GetObjectText", 2}, {"SpawnDerivedClass", 2},
    {"SpawnInstance", 2}, {"CompareTo", 2}, {"GetPropertyOrigin", 2}, {"InheritsFrom", 1}, {"GetMethod", 4},
    {"PutMethod", 4}, {"DeleteMethod", 1}, {"BeginMethodEnumeration", 1}, {"NextMethod", 4},
    {"EndMethodEnumeration", 0}, {"GetMethodQualifierSet", 2}, {"GetMethodOrigin", 2}
};

#define GWB_GUID(a, b, c, d1, d2, d3, d4, d5, d6, d7, d8) \
    { (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16), (uint8_t)((a) >> 24), (uint8_t)(b), \
      (uint8_t)((b) >> 8), (uint8_t)(c), (uint8_t)((c) >> 8), d1, d2, d3, d4, d5, d6, d7, d8 }

static const struct {
    const char *name;
    uint8_t iid[16];
    const GwbMethod *methods;
    uint32_t count;
} gwb_interfaces[GWB_KIND_COUNT] = {
    { "IWbemLocator", GWB_GUID(0xdc12a687u, 0x737fu, 0x11cfu, 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24),
      locator_methods, sizeof locator_methods / sizeof locator_methods[0] },
    { "IWbemServices", GWB_GUID(0x9556dc99u, 0x828cu, 0x11cfu, 0xa3, 0x7e, 0x00, 0xaa, 0x00, 0x32, 0x40, 0xc7),
      services_methods, sizeof services_methods / sizeof services_methods[0] },
    { "IEnumWbemClassObject", GWB_GUID(0x027947e1u, 0xd731u, 0x11ceu, 0xa3, 0x57, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01),
      enum_methods, sizeof enum_methods / sizeof enum_methods[0] },
    { "IWbemClassObject", GWB_GUID(0xdc12a681u, 0x737fu, 0x11cfu, 0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24),
      object_methods, sizeof object_methods / sizeof object_methods[0] }
};
const uint8_t gwb_clsid_locator[16] =
    GWB_GUID(0x4590f811u, 0x1d3au, 0x11d0u, 0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24);
static const uint8_t gwb_iunknown[16] =
    GWB_GUID(0x00000000u, 0x0000u, 0x0000u, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

typedef struct {
    int used;
    uint32_t kind, refcount;
    uint32_t position;   /* enumerator: next entity */
    uint32_t entity;     /* class object: entity index */
} GwbObject;

struct GuestWbem {
    GuestMemory *memory;
    uint32_t arena;
    GwbObject objects[GWB_MAX_OBJECTS];
    uint32_t live;
    char entities[GWB_MAX_ENTITIES][128];
    uint32_t entity_count;
    GwbBstrAlloc bstr_alloc;
    void *bstr_context;
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static int write_u32(GuestWbem *wb, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(wb->memory, address, b, 4);
}
static uint32_t trap_of(uint32_t kind, uint32_t slot) {
    return GWB_TRAP_BASE + (kind * GWB_SLOTS_PER_INTERFACE + slot) * GWB_TRAP_STRIDE;
}
static uint32_t vtable_of(const GuestWbem *wb, uint32_t kind) {
    return wb->arena + kind * GWB_SLOTS_PER_INTERFACE * 4u;
}
static uint32_t object_address(const GuestWbem *wb, uint32_t index) {
    return wb->arena + GWB_KIND_COUNT * GWB_SLOTS_PER_INTERFACE * 4u + index * 16u;
}

int gwb_create(GuestMemory *memory, GuestWbem **out) {
    if (!memory || !out) return GM_RANGE;
    GuestWbem *wb = (GuestWbem *)calloc(1, sizeof *wb);
    if (!wb) return GM_ALLOCATION;
    wb->memory = memory;
    *out = wb;
    return GM_OK;
}
void gwb_destroy(GuestWbem *wb) { free(wb); }

int gwb_set_arena(GuestWbem *wb, uint32_t base, uint32_t size) {
    if (!wb || size < GWB_ARENA_MIN) return GU_STOP_WBEM_BAD_ARGS;
    int rc = gm_check(wb->memory, base, size, GM_WRITE);
    if (rc) return rc;
    wb->arena = base;
    for (uint32_t kind = 0; kind < GWB_KIND_COUNT; kind++)
        for (uint32_t slot = 0; slot < GWB_SLOTS_PER_INTERFACE; slot++) {
            rc = write_u32(wb, vtable_of(wb, kind) + slot * 4u, trap_of(kind, slot));
            if (rc) return rc;
        }
    return GM_OK;
}
void gwb_set_bstr_alloc(GuestWbem *wb, GwbBstrAlloc alloc, void *context) {
    if (!wb) return;
    wb->bstr_alloc = alloc;
    wb->bstr_context = context;
}
int gwb_set_pnp_entities(GuestWbem *wb, const char *const *device_ids, uint32_t count) {
    if (!wb || count > GWB_MAX_ENTITIES || (count && !device_ids)) return GU_STOP_WBEM_BAD_ARGS;
    for (uint32_t i = 0; i < count; i++) {
        if (!device_ids[i] || strlen(device_ids[i]) >= sizeof wb->entities[i]) return GU_STOP_WBEM_BAD_ARGS;
        snprintf(wb->entities[i], sizeof wb->entities[i], "%s", device_ids[i]);
    }
    wb->entity_count = count;
    return GM_OK;
}
uint32_t gwb_live_objects(const GuestWbem *wb) { return wb ? wb->live : 0u; }

static int create_object(GuestWbem *wb, uint32_t kind, uint32_t *address, GwbObject **record) {
    if (!wb->arena) return GU_STOP_WBEM_NO_ARENA;
    for (uint32_t i = 0; i < GWB_MAX_OBJECTS; i++) {
        if (wb->objects[i].used) continue;
        uint8_t block[16];
        put32(block + 0, vtable_of(wb, kind));
        put32(block + 4, 1);
        put32(block + 8, kind);
        put32(block + 12, 0x314d4257u); /* "WBM1" */
        int rc = gm_write(wb->memory, object_address(wb, i), block, sizeof block);
        if (rc) return rc;
        memset(&wb->objects[i], 0, sizeof wb->objects[i]);
        wb->objects[i].used = 1;
        wb->objects[i].kind = kind;
        wb->objects[i].refcount = 1;
        wb->live++;
        *address = object_address(wb, i);
        if (record) *record = &wb->objects[i];
        return GM_OK;
    }
    *address = 0;   /* the caller answers WBEM_E_OUT_OF_MEMORY */
    if (record) *record = NULL;
    return GM_OK;
}
static GwbObject *find_object(GuestWbem *wb, uint32_t address) {
    if (!wb->arena || address < object_address(wb, 0)) return NULL;
    uint32_t delta = address - object_address(wb, 0);
    if (delta % 16u || delta / 16u >= GWB_MAX_OBJECTS) return NULL;
    GwbObject *record = &wb->objects[delta / 16u];
    return record->used ? record : NULL;
}

/* Reads a NUL-terminated UTF-16 string as lower-case ASCII; -1 when it is not
 * plain ASCII or longer than the buffer. */
static int read_ascii_lower(GuestWbem *wb, uint32_t address, char *out, uint32_t capacity) {
    if (!address) return -1;
    for (uint32_t i = 0; i < capacity; i++) {
        uint8_t unit[2];
        if (gm_read(wb->memory, address + i * 2u, unit, 2)) return -1;
        if (unit[1] || unit[0] > 0x7e) return -1;
        char c = (char)unit[0];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
        if (!c) return 0;
    }
    return -1;
}

int gwb_factory(void *context, const uint8_t clsid[16], const uint8_t iid[16], uint32_t outer,
                uint32_t *object, uint32_t *hresult) {
    GuestWbem *wb = (GuestWbem *)context;
    (void)outer;
    if (!wb || !object || !hresult) return GU_STOP_WBEM_BAD_ARGS;
    if (memcmp(clsid, gwb_clsid_locator, 16)) return GU_STOP_WBEM_BAD_ARGS;
    *object = 0;
    if (memcmp(iid, gwb_interfaces[GWB_LOCATOR].iid, 16) && memcmp(iid, gwb_iunknown, 16)) {
        *hresult = GWB_E_NOINTERFACE;
        return GM_OK;
    }
    int rc = create_object(wb, GWB_LOCATOR, object, NULL);
    if (rc) return rc;
    *hresult = *object ? GWB_S_OK : GWB_E_OUT_OF_MEMORY;
    return GM_OK;
}

static int connect_server(GuestWbem *wb, const uint32_t *args, uint32_t *result) {
    /* (this, strNetworkResource, strUser, strPassword, strLocale, lSecurityFlags, strAuthority, pCtx, ppNamespace) */
    if (!args[8]) { *result = GWB_E_INVALID_PARAMETER; return GM_OK; }
    int rc = write_u32(wb, args[8], 0);
    if (rc) return rc;
    char name[64];
    const char *path = name;
    if (read_ascii_lower(wb, args[1], name, sizeof name)) { *result = GWB_E_INVALID_NAMESPACE; return GM_OK; }
    if (!strncmp(path, "\\\\.\\", 4)) path += 4;
    if (strcmp(path, "root\\cimv2")) {
        fprintf(stderr, "wbem: ConnectServer namespace '%s' is not part of this repository\n", name);
        *result = GWB_E_INVALID_NAMESPACE;
        return GM_OK;
    }
    uint32_t services = 0;
    rc = create_object(wb, GWB_SERVICES, &services, NULL);
    if (rc) return rc;
    if (!services) { *result = GWB_E_OUT_OF_MEMORY; return GM_OK; }
    rc = write_u32(wb, args[8], services);
    if (rc) return rc;
    *result = GWB_S_OK;
    return GM_OK;
}

static int create_instance_enum(GuestWbem *wb, const uint32_t *args, uint32_t *result) {
    /* (this, strFilter, lFlags, pCtx, ppEnum) */
    if (!args[4]) { *result = GWB_E_INVALID_PARAMETER; return GM_OK; }
    int rc = write_u32(wb, args[4], 0);
    if (rc) return rc;
    char name[64];
    if (read_ascii_lower(wb, args[1], name, sizeof name) || strcmp(name, "win32_pnpentity")) {
        fprintf(stderr, "wbem: CreateInstanceEnum class is not part of this repository\n");
        *result = GWB_E_INVALID_CLASS;
        return GM_OK;
    }
    uint32_t enumerator = 0;
    rc = create_object(wb, GWB_ENUM, &enumerator, NULL);
    if (rc) return rc;
    if (!enumerator) { *result = GWB_E_OUT_OF_MEMORY; return GM_OK; }
    rc = write_u32(wb, args[4], enumerator);
    if (rc) return rc;
    *result = GWB_S_OK;
    return GM_OK;
}

static int exec_query(GuestWbem *wb, const uint32_t *args, uint32_t *result) {
    /* (this, strQueryLanguage, strQuery, lFlags, pCtx, ppEnum) */
    if (args[5]) {
        int rc = write_u32(wb, args[5], 0);
        if (rc) return rc;
    }
    *result = GWB_E_NOT_SUPPORTED;
    return GM_OK;
}

static int enum_next(GuestWbem *wb, GwbObject *record, const uint32_t *args, uint32_t *result) {
    /* (this, lTimeout, uCount, apObjects, puReturned) */
    uint32_t wanted = args[2];
    if (!args[4] || (wanted && !args[3])) { *result = GWB_E_INVALID_PARAMETER; return GM_OK; }
    uint32_t returned = 0;
    while (returned < wanted && record->position < wb->entity_count) {
        uint32_t object = 0;
        GwbObject *instance = NULL;
        int rc = create_object(wb, GWB_OBJECT, &object, &instance);
        if (rc) return rc;
        if (!object) break;   /* object table full: a short batch, the caller asks again */
        instance->entity = record->position++;
        rc = write_u32(wb, args[3] + returned * 4u, object);
        if (rc) return rc;
        returned++;
    }
    int rc = write_u32(wb, args[4], returned);
    if (rc) return rc;
    *result = returned == wanted ? GWB_S_OK : GWB_S_FALSE;
    return GM_OK;
}

static int object_get(GuestWbem *wb, GwbObject *record, const uint32_t *args, uint32_t *result) {
    /* (this, wszName, lFlags, pVal, pType, plFlavor) */
    if (args[2]) { *result = GWB_E_INVALID_PARAMETER; return GM_OK; }
    char name[64];
    if (read_ascii_lower(wb, args[1], name, sizeof name) || strcmp(name, "deviceid")) {
        *result = GWB_E_NOT_FOUND;
        return GM_OK;
    }
    if (args[5]) {
        fprintf(stderr, "wbem-stop: IWbemClassObject::Get property flavor is not modelled\n");
        return GU_STOP_WBEM_UNIMPLEMENTED;
    }
    if (record->entity >= wb->entity_count) return GU_STOP_WBEM_BAD_OBJECT;
    if (args[3]) {
        if (!wb->bstr_alloc) return GU_STOP_WBEM_NO_BSTR;
        uint32_t bstr = 0;
        int rc = wb->bstr_alloc(wb->bstr_context, wb->entities[record->entity], &bstr);
        if (rc) return rc;
        if (!bstr) { *result = GWB_E_OUT_OF_MEMORY; return GM_OK; }
        uint8_t variant[16];
        memset(variant, 0, sizeof variant);
        variant[0] = 8;   /* VT_BSTR */
        put32(variant + 8, bstr);
        rc = gm_write(wb->memory, args[3], variant, sizeof variant);
        if (rc) return rc;
    }
    if (args[4]) {
        int rc = write_u32(wb, args[4], 8);   /* CIM_STRING */
        if (rc) return rc;
    }
    *result = GWB_S_OK;
    return GM_OK;
}

int gwb_call(GuestWbem *wb, uint32_t trap, const uint32_t *args, uint32_t *result, const char **what) {
    static const char *bad = "wbem-bad-trap";
    if (what) *what = bad;
    if (!wb || !args || !result || !gwb_is_trap(trap) || (trap - GWB_TRAP_BASE) % GWB_TRAP_STRIDE)
        return GU_STOP_WBEM_BAD_OBJECT;
    uint32_t index = (trap - GWB_TRAP_BASE) / GWB_TRAP_STRIDE;
    uint32_t kind = index / GWB_SLOTS_PER_INTERFACE, slot = index % GWB_SLOTS_PER_INTERFACE;
    if (kind >= GWB_KIND_COUNT || slot >= gwb_interfaces[kind].count) return GU_STOP_WBEM_BAD_OBJECT;
    static _Thread_local char text[80];
    snprintf(text, sizeof text, "%s::%s", gwb_interfaces[kind].name, gwb_interfaces[kind].methods[slot].name);
    if (what) *what = text;
    GwbObject *record = find_object(wb, args[0]);
    if (!record || record->kind != kind) return GU_STOP_WBEM_BAD_OBJECT;
    int rc = GU_STOP_WBEM_UNIMPLEMENTED;
    if (slot == 0) {
        uint8_t iid[16];
        rc = gm_read(wb->memory, args[1], iid, sizeof iid);
        if (!rc && (!memcmp(iid, gwb_iunknown, 16) || !memcmp(iid, gwb_interfaces[kind].iid, 16))) {
            record->refcount++;
            rc = write_u32(wb, args[0] + 4, record->refcount);
            if (!rc) rc = write_u32(wb, args[2], args[0]);
            *result = GWB_S_OK;
        } else if (!rc) {
            rc = write_u32(wb, args[2], 0);
            *result = GWB_E_NOINTERFACE;
        }
    } else if (slot == 1) {
        record->refcount++;
        rc = write_u32(wb, args[0] + 4, record->refcount);
        *result = record->refcount;
    } else if (slot == 2) {
        record->refcount--;
        rc = write_u32(wb, args[0] + 4, record->refcount);
        *result = record->refcount;
        if (!record->refcount) {
            record->used = 0;
            if (wb->live) wb->live--;
        }
    } else if (kind == GWB_LOCATOR && slot == 3) {
        rc = connect_server(wb, args, result);
    } else if (kind == GWB_SERVICES && slot == 18) {
        rc = create_instance_enum(wb, args, result);
    } else if (kind == GWB_SERVICES && slot == 20) {
        rc = exec_query(wb, args, result);
    } else if (kind == GWB_ENUM && slot == 3) {
        record->position = 0;
        *result = GWB_S_OK;
        rc = GM_OK;
    } else if (kind == GWB_ENUM && slot == 4) {
        rc = enum_next(wb, record, args, result);
    } else if (kind == GWB_OBJECT && slot == 4) {
        rc = object_get(wb, record, args, result);
    }
    if (rc == GU_STOP_WBEM_UNIMPLEMENTED) fprintf(stderr, "wbem-unimplemented: %s\n", text);
    if (rc) return rc < 0 ? rc : -rc;
    return (int)(4u + 4u * gwb_interfaces[kind].methods[slot].args);
}
