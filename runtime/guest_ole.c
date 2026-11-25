/* ole32/oleaut32 bridges: apartment counter, task allocator, CLSID factory
 * table, BSTR and VariantClear. See guest_ole.h for the modelled scope.
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write; guest
 * allocations live on the port-owned process heap, so no host pointer is ever
 * visible to the guest. A CLSID that no host factory registered is answered
 * with the documented REGDB_E_CLASSNOTREG, never with a fabricated object. */
#include "guest_ole.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int used;
    uint8_t clsid[16];
    GolFactory factory;
    void *context;
} GolFactoryEntry;

typedef struct {
    uint32_t address;
    uint32_t length; /* bytes, as stored in the BSTR prefix */
} GolBstr;

struct GuestOle {
    GuestMemory *memory;
    uint32_t teb;
    GuestProcessHeap *heap;
    uint32_t apartment_count;
    uint32_t apartment_model;
    GolFactoryEntry factories[GOL_MAX_FACTORIES];
    GolBstr bstrs[GOL_MAX_BSTR];
    uint32_t bstr_live;
    uint8_t unregistered[GOL_MAX_UNREGISTERED][32];
    uint32_t unregistered_count;
    GolGuestRelease release;
    void *release_context;
    char last_stop[96];
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}
static int write_u32(GuestOle *ole, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(ole->memory, address, b, 4);
}
static int stop(GuestOle *ole, int code, const char *what) {
    snprintf(ole->last_stop, sizeof(ole->last_stop), "%s", what);
    fprintf(stderr, "ole-stop: %s (%d)\n", what, code);
    return code;
}

int gol_create(GuestMemory *memory, uint32_t teb, GuestProcessHeap *heap,
               GuestOle **out) {
    if (!memory || !out) return GM_RANGE;
    GuestOle *ole = (GuestOle *)calloc(1, sizeof(*ole));
    if (!ole) return GM_ALLOCATION;
    ole->memory = memory;
    ole->teb = teb;
    ole->heap = heap;
    *out = ole;
    return GM_OK;
}
void gol_destroy(GuestOle *ole) { free(ole); }

int gol_register_factory(GuestOle *ole, const uint8_t clsid[16],
                         GolFactory factory, void *context) {
    if (!ole || !clsid || !factory) return GM_UNSUPPORTED;
    for (uint32_t i = 0; i < GOL_MAX_FACTORIES; i++) {
        GolFactoryEntry *entry = &ole->factories[i];
        if (!entry->used) {
            entry->used = 1;
            memcpy(entry->clsid, clsid, 16);
            entry->factory = factory;
            entry->context = context;
            return GM_OK;
        }
    }
    return GM_UNSUPPORTED;
}
void gol_set_release_hook(GuestOle *ole, GolGuestRelease release,
                          void *context) {
    if (!ole) return;
    ole->release = release;
    ole->release_context = context;
}

uint32_t gol_apartment_count(const GuestOle *ole) {
    return ole ? ole->apartment_count : 0u;
}
uint32_t gol_apartment_model(const GuestOle *ole) {
    return ole ? ole->apartment_model : 0u;
}
uint32_t gol_bstr_live(const GuestOle *ole) { return ole ? ole->bstr_live : 0u; }
uint32_t gol_unregistered(const GuestOle *ole) {
    return ole ? ole->unregistered_count : 0u;
}
int gol_unregistered_at(const GuestOle *ole, uint32_t index, uint8_t clsid[16],
                        uint8_t iid[16]) {
    if (!ole || !clsid || !iid || index >= ole->unregistered_count)
        return GM_UNSUPPORTED;
    memcpy(clsid, ole->unregistered[index], 16);
    memcpy(iid, ole->unregistered[index] + 16, 16);
    return GM_OK;
}
const char *gol_last_stop(const GuestOle *ole) {
    return (ole && ole->last_stop[0]) ? ole->last_stop : NULL;
}
const char *gol_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_OLE_LOCK_RUNNING: return "ole-lock-running-unsupported";
    case GU_STOP_OLE_FONT_INDIRECT: return "oleaut32-font-indirect-unsupported";
    case GU_STOP_OLE_BAD_BSTR: return "oleaut32-bstr-not-issued-here";
    case GU_STOP_OLE_VARIANT_TYPE: return "oleaut32-variant-type-unsupported";
    case GU_STOP_OLE_NO_RELEASE_HOOK: return "oleaut32-no-guest-release-hook";
    case GU_STOP_OLE_FACTORY: return "ole32-factory-callback-failed";
    case GU_STOP_OLE_BAD_ARGS: return "ole32-argument-outside-model";
    case GU_STOP_OLE_NO_HEAP: return "ole32-guest-process-heap-missing";
    default: return "unknown-stop";
    }
}

/* --- BSTR bookkeeping --------------------------------------------------- */
static GolBstr *bstr_find(GuestOle *ole, uint32_t address) {
    for (uint32_t i = 0; i < GOL_MAX_BSTR; i++)
        if (ole->bstrs[i].address == address) return &ole->bstrs[i];
    return NULL;
}
static GolBstr *bstr_slot(GuestOle *ole) {
    for (uint32_t i = 0; i < GOL_MAX_BSTR; i++)
        if (!ole->bstrs[i].address) return &ole->bstrs[i];
    return NULL;
}
/* Allocates 4 + (units+1)*2 guest bytes and writes the documented prefix. */
static int bstr_alloc(GuestOle *ole, uint32_t units, uint32_t *out) {
    if (!ole->heap) return GU_STOP_OLE_NO_HEAP;
    GolBstr *record = bstr_slot(ole);
    if (!record) return GU_STOP_OLE_BAD_BSTR;
    uint32_t bytes = 4u + (units + 1u) * 2u;
    uint32_t address = 0;
    int rc = gph_alloc(ole->heap, GPH_HANDLE, 0, bytes, &address);
    if (rc) return rc;
    if (!address) { *out = 0; return GM_OK; }
    uint8_t prefix[4];
    put32(prefix, units * 2u);
    rc = gm_write(ole->memory, address, prefix, 4);
    if (rc) { (void)gph_free(ole->heap, GPH_HANDLE, 0, address); return rc; }
    record->address = address;
    record->length = units * 2u;
    ole->bstr_live++;
    *out = address + 4u; /* the BSTR points past the prefix */
    return GM_OK;
}
static int bstr_free(GuestOle *ole, uint32_t bstr, uint32_t *result) {
    if (!bstr) { *result = 0; return GM_OK; }
    if (!ole->heap) return GU_STOP_OLE_NO_HEAP;
    GolBstr *record = bstr_find(ole, bstr - 4u);
    if (!record) return stop(ole, GU_STOP_OLE_BAD_BSTR, "SysFreeString: pointer "
                             "was not issued by this runtime");
    int rc = gph_free(ole->heap, GPH_HANDLE, 0, record->address);
    if (rc) return rc;
    record->address = 0;
    record->length = 0;
    if (ole->bstr_live) ole->bstr_live--;
    *result = 0;
    return GM_OK;
}
static int bstr_len(GuestOle *ole, uint32_t bstr, uint32_t *units) {
    if (!bstr) { *units = 0; return GM_OK; }
    uint8_t prefix[4];
    int rc = gm_read(ole->memory, bstr - 4u, prefix, 4);
    if (rc) return rc;
    *units = get32(prefix) / 2u;
    return GM_OK;
}

/* --- ole32 ------------------------------------------------------------- */
static int co_initialize(GuestOle *ole, uint32_t reserved, uint32_t model,
                         uint32_t *result) {
    if (reserved) { *result = GOL_E_INVALIDARG; return GM_OK; }
    if (model & ~GOL_COINIT_KNOWN_MASK) {
        *result = GOL_E_INVALIDARG;
        return GM_OK;
    }
    uint32_t requested = model & GOL_COINIT_APARTMENTTHREADED;
    if (!ole->apartment_count) {
        ole->apartment_model = requested;
        ole->apartment_count = 1;
        *result = GOL_S_OK;
        return GM_OK;
    }
    if (ole->apartment_model != requested) {
        *result = GOL_RPC_E_CHANGED_MODE;
        return GM_OK;
    }
    if (ole->apartment_count == UINT32_MAX) return GM_UNSUPPORTED;
    ole->apartment_count++;
    *result = GOL_S_FALSE;
    return GM_OK;
}
int gol_co_initialize(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    return co_initialize(ole, args[0], GOL_COINIT_APARTMENTTHREADED, result);
}
int gol_co_initialize_ex(GuestOle *ole, const uint32_t *args,
                         uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    return co_initialize(ole, args[0], args[1], result);
}
int gol_co_uninitialize(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    (void)args;
    if (!ole || !result) return GU_STOP_OLE_BAD_ARGS;
    /* Documented: a call without a matching CoInitialize is ignored. */
    if (ole->apartment_count) ole->apartment_count--;
    if (!ole->apartment_count) ole->apartment_model = 0;
    *result = 0;
    return GM_OK;
}
int gol_co_task_mem_alloc(GuestOle *ole, const uint32_t *args,
                          uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    if (!ole->heap) return GU_STOP_OLE_NO_HEAP;
    uint32_t size = args[0];
    uint32_t address = 0;
    /* CoTaskMemAlloc(0) returns a unique freeable pointer, so allocate one byte
     * worth of block instead of failing. */
    int rc = gph_alloc(ole->heap, GPH_HANDLE, 0, size ? size : 1u, &address);
    if (rc) return rc;
    *result = address;
    return GM_OK;
}
int gol_co_task_mem_realloc(GuestOle *ole, const uint32_t *args,
                            uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    if (!ole->heap) return GU_STOP_OLE_NO_HEAP;
    if (!args[0]) return gol_co_task_mem_alloc(ole, args, result);
    uint32_t moved = 0;
    int rc = gph_realloc(ole->heap, GPH_HANDLE, 0, args[0],
                         args[1] ? args[1] : 1u, &moved);
    if (rc) return rc;
    *result = moved;
    return GM_OK;
}
int gol_co_task_mem_free(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    if (!args[0]) { *result = 0; return GM_OK; }
    if (!ole->heap) return GU_STOP_OLE_NO_HEAP;
    int rc = gph_free(ole->heap, GPH_HANDLE, 0, args[0]);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}
static void log_guid(const char *what, const uint8_t *guid) {
    fprintf(stderr, "%s={%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}\n",
            what, get32(guid), (unsigned)(guid[4] | (guid[5] << 8)),
            (unsigned)(guid[6] | (guid[7] << 8)), guid[8], guid[9], guid[10],
            guid[11], guid[12], guid[13], guid[14], guid[15]);
}
int gol_co_create_instance(GuestOle *ole, const uint32_t *args,
                           uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    uint8_t clsid[16], iid[16];
    int rc = gm_read(ole->memory, args[0], clsid, 16);
    if (rc) return rc;
    rc = gm_read(ole->memory, args[3], iid, 16);
    if (rc) return rc;
    uint32_t context = args[2];
    if (context & ~GOL_CLSCTX_ALL) return GU_STOP_OLE_BAD_ARGS;
    rc = write_u32(ole, args[4], 0); /* *ppv = NULL first */
    if (rc) return rc;
    if (args[1]) {
        /* Aggregation needs a real controlling IUnknown; not modelled. */
        *result = GOL_CLASS_E_NOAGGREGATION;
        return GM_OK;
    }
    for (uint32_t i = 0; i < GOL_MAX_FACTORIES; i++) {
        GolFactoryEntry *entry = &ole->factories[i];
        if (!entry->used || memcmp(entry->clsid, clsid, 16) != 0) continue;
        uint32_t object = 0, hresult = GOL_E_FAIL;
        rc = entry->factory(entry->context, clsid, iid, args[1], &object,
                            &hresult);
        if (rc) return stop(ole, GU_STOP_OLE_FACTORY,
                            "CoCreateInstance: factory callback failed");
        if ((int32_t)hresult >= 0 && object) {
            rc = write_u32(ole, args[4], object);
            if (rc) return rc;
        }
        *result = hresult;
        return GM_OK;
    }
    /* Not registered: the documented REGDB_E_CLASSNOTREG, with the GUIDs
     * logged so the missing server can be identified from the trace. */
    if (ole->unregistered_count < GOL_MAX_UNREGISTERED) {
        memcpy(ole->unregistered[ole->unregistered_count], clsid, 16);
        memcpy(ole->unregistered[ole->unregistered_count] + 16, iid, 16);
        ole->unregistered_count++;
    }
    fprintf(stderr, "ole32: CoCreateInstance class not registered ");
    log_guid("clsid", clsid);
    log_guid("iid", iid);
    *result = GOL_REGDB_E_CLASSNOTREG;
    return GM_OK;
}
int gol_co_initialize_security(GuestOle *ole, const uint32_t *args,
                               uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    /* No DCOM, no remote endpoint, no authentication in this process. */
    *result = GOL_S_OK;
    return GM_OK;
}
int gol_co_set_proxy_blanket(GuestOle *ole, const uint32_t *args,
                             uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    /* Process-local proxies only: the documented "no change" success. */
    *result = GOL_S_OK;
    return GM_OK;
}
int gol_co_free_unused_libraries(GuestOle *ole, const uint32_t *args,
                                 uint32_t *result) {
    (void)args;
    if (!ole || !result) return GU_STOP_OLE_BAD_ARGS;
    *result = 0;
    return GM_OK;
}
int gol_ole_lock_running(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    (void)args;
    if (!ole || !result) return GU_STOP_OLE_BAD_ARGS;
    return stop(ole, GU_STOP_OLE_LOCK_RUNNING,
                "OleLockRunning: no running-object table in this runtime");
}

/* --- oleaut32 ---------------------------------------------------------- */
int gol_sys_alloc_string(GuestOle *ole, const uint32_t *args,
                         uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    if (!args[0]) { *result = 0; return GM_OK; }
    uint32_t units = 0;
    for (;;) {
        uint8_t bytes[2];
        int rc = gm_read(ole->memory, args[0] + units * 2u, bytes, 2);
        if (rc) return rc;
        if (!bytes[0] && !bytes[1]) break;
        units++;
        if (units > 0x1000000u) return GU_STOP_OLE_BAD_ARGS;
    }
    uint32_t bstr = 0;
    int rc = bstr_alloc(ole, units, &bstr);
    if (rc) return rc;
    if (!bstr) { *result = 0; return GM_OK; }
    if (units) {
        rc = gm_check(ole->memory, args[0], units * 2u, GM_READ);
        if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
        uint8_t chunk[512];
        uint32_t done = 0;
        while (done < units * 2u) {
            uint32_t step = units * 2u - done;
            if (step > sizeof(chunk)) step = sizeof(chunk);
            rc = gm_read(ole->memory, args[0] + done, chunk, step);
            if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
            rc = gm_write(ole->memory, bstr + done, chunk, step);
            if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
            done += step;
        }
    }
    uint8_t nul[2] = {0, 0};
    rc = gm_write(ole->memory, bstr + units * 2u, nul, 2);
    if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
    *result = bstr;
    return GM_OK;
}
int gol_sys_alloc_string_len(GuestOle *ole, const uint32_t *args,
                             uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    uint32_t units = args[1];
    if (units > 0x1000000u) return GU_STOP_OLE_BAD_ARGS;
    uint32_t bstr = 0;
    int rc = bstr_alloc(ole, units, &bstr);
    if (rc) return rc;
    if (!bstr) { *result = 0; return GM_OK; }
    if (args[0] && units) {
        rc = gm_check(ole->memory, args[0], units * 2u, GM_READ);
        if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
        uint8_t chunk[512];
        uint32_t done = 0;
        while (done < units * 2u) {
            uint32_t step = units * 2u - done;
            if (step > sizeof(chunk)) step = sizeof(chunk);
            rc = gm_read(ole->memory, args[0] + done, chunk, step);
            if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
            rc = gm_write(ole->memory, bstr + done, chunk, step);
            if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
            done += step;
        }
    }
    /* SysAllocStringLen(NULL, n) leaves the units uninitialised on Windows;
     * this runtime hands out deterministic zero bytes (documented deviation). */
    uint8_t nul[2] = {0, 0};
    rc = gm_write(ole->memory, bstr + units * 2u, nul, 2);
    if (rc) { (void)bstr_free(ole, bstr, result); return rc; }
    *result = bstr;
    return GM_OK;
}
/* Host-side BSTR for runtime COM objects (runtime/guest_wbem.c): the same
 * tracked allocation SysAllocString hands out, filled from an ASCII string. */
int gol_alloc_bstr_ascii(GuestOle *ole, const char *text, uint32_t *bstr) {
    if (!ole || !text || !bstr) return GU_STOP_OLE_BAD_ARGS;
    uint32_t units = (uint32_t)strlen(text);
    if (units > 0x1000u) return GU_STOP_OLE_BAD_ARGS;
    *bstr = 0;
    uint32_t address = 0;
    int rc = bstr_alloc(ole, units, &address);
    if (rc) return rc;
    if (!address) return GM_OK;
    for (uint32_t i = 0; i <= units; i++) {
        uint8_t unit[2] = { i < units ? (uint8_t)text[i] : 0, 0 };
        rc = gm_write(ole->memory, address + i * 2u, unit, 2);
        if (rc) { uint32_t ignored = 0; (void)bstr_free(ole, address, &ignored); return rc; }
    }
    *bstr = address;
    return GM_OK;
}
int gol_sys_free_string(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    return bstr_free(ole, args[0], result);
}
int gol_sys_string_len(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    uint32_t units = 0;
    int rc = bstr_len(ole, args[0], &units);
    if (rc) return rc;
    *result = units;
    return GM_OK;
}
int gol_variant_clear(GuestOle *ole, const uint32_t *args, uint32_t *result) {
    if (!ole || !args || !result) return GU_STOP_OLE_BAD_ARGS;
    if (!args[0]) { *result = GOL_E_INVALIDARG; return GM_OK; }
    uint8_t variant[GOL_VARIANT_SIZE];
    int rc = gm_read(ole->memory, args[0], variant, sizeof(variant));
    if (rc) return rc;
    uint32_t vt = (uint32_t)variant[0] | ((uint32_t)variant[1] << 8);
    if (vt & (GOL_VT_ARRAY | GOL_VT_BYREF)) {
        snprintf(ole->last_stop, sizeof(ole->last_stop),
                 "VariantClear: vt=%#x by-ref/array unsupported", vt);
        return stop(ole, GU_STOP_OLE_VARIANT_TYPE,
                    "VariantClear: by-ref/array variant unsupported");
    }
    uint32_t data = get32(variant + 8);
    switch (vt) {
    case GOL_VT_EMPTY:
    case GOL_VT_NULL:
    case GOL_VT_I2:
    case GOL_VT_I4:
    case GOL_VT_R4:
    case GOL_VT_R8:
    case GOL_VT_CY:
    case GOL_VT_DATE:
    case GOL_VT_ERROR:
    case GOL_VT_BOOL:
    case GOL_VT_DECIMAL:
    case GOL_VT_I1:
    case GOL_VT_UI1:
    case GOL_VT_UI2:
    case GOL_VT_UI4:
    case GOL_VT_I8:
    case GOL_VT_UI8:
    case GOL_VT_INT:
    case GOL_VT_UINT:
        break;
    case GOL_VT_BSTR:
        if (data) {
            rc = bstr_free(ole, data, result);
            if (rc) return rc;
        }
        break;
    case GOL_VT_DISPATCH:
    case GOL_VT_UNKNOWN:
        if (data) {
            if (!ole->release)
                return stop(ole, GU_STOP_OLE_NO_RELEASE_HOOK,
                            "VariantClear: VT_DISPATCH/UNKNOWN without a "
                            "guest Release hook");
            uint32_t hresult = 0;
            rc = ole->release(ole->release_context, data, &hresult);
            if (rc) return rc;
        }
        break;
    default:
        snprintf(ole->last_stop, sizeof(ole->last_stop),
                 "VariantClear: vt=%#x unsupported", vt);
        return stop(ole, GU_STOP_OLE_VARIANT_TYPE,
                    "VariantClear: variant type unsupported");
    }
    uint8_t cleared[GOL_VARIANT_SIZE];
    memset(cleared, 0, sizeof(cleared));
    rc = gm_write(ole->memory, args[0], cleared, sizeof(cleared));
    if (rc) return rc;
    *result = GOL_S_OK;
    return GM_OK;
}
int gol_ole_create_font_indirect(GuestOle *ole, const uint32_t *args,
                                 uint32_t *result) {
    (void)args;
    if (!ole || !result) return GU_STOP_OLE_BAD_ARGS;
    return stop(ole, GU_STOP_OLE_FONT_INDIRECT,
                "OleCreateFontIndirect: no IFont/StdFont object exists here");
}

/* --- registration lists ------------------------------------------------- */
#define GOL_ARGS(n) ((n) * 4u)
const GuestOleExport ole32_exports[] = {
    { "ole32.dll", "CoInitialize", GOL_ARGS(1), gol_co_initialize },
    { "ole32.dll", "CoInitializeEx", GOL_ARGS(2), gol_co_initialize_ex },
    { "ole32.dll", "CoUninitialize", GOL_ARGS(0), gol_co_uninitialize },
    { "ole32.dll", "CoTaskMemAlloc", GOL_ARGS(1), gol_co_task_mem_alloc },
    { "ole32.dll", "CoTaskMemRealloc", GOL_ARGS(2), gol_co_task_mem_realloc },
    { "ole32.dll", "CoTaskMemFree", GOL_ARGS(1), gol_co_task_mem_free },
    { "ole32.dll", "CoCreateInstance", GOL_ARGS(5), gol_co_create_instance },
    { "ole32.dll", "CoInitializeSecurity", GOL_ARGS(9),   /* 9 parameters (objbase.h) */
      gol_co_initialize_security },
    { "ole32.dll", "CoSetProxyBlanket", GOL_ARGS(8), gol_co_set_proxy_blanket },   /* 8 parameters */
    { "ole32.dll", "CoFreeUnusedLibraries", GOL_ARGS(0),
      gol_co_free_unused_libraries },
    { "ole32.dll", "OleLockRunning", GOL_ARGS(3), gol_ole_lock_running }
};
const uint32_t ole32_export_count =
    (uint32_t)(sizeof(ole32_exports) / sizeof(ole32_exports[0]));

const GuestOleExport oleaut32_exports[] = {
    { "oleaut32.dll", "SysAllocString", GOL_ARGS(1), gol_sys_alloc_string },
    { "oleaut32.dll", "SysAllocStringLen", GOL_ARGS(2),
      gol_sys_alloc_string_len },
    { "oleaut32.dll", "SysFreeString", GOL_ARGS(1), gol_sys_free_string },
    { "oleaut32.dll", "SysStringLen", GOL_ARGS(1), gol_sys_string_len },
    { "oleaut32.dll", "VariantClear", GOL_ARGS(1), gol_variant_clear },
    { "oleaut32.dll", "OleCreateFontIndirect", GOL_ARGS(3),
      gol_ole_create_font_indirect }
};
const uint32_t oleaut32_export_count =
    (uint32_t)(sizeof(oleaut32_exports) / sizeof(oleaut32_exports[0]));
