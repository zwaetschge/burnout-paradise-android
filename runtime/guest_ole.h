#ifndef BURNOUT_GUEST_OLE_H
#define BURNOUT_GUEST_OLE_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_process_heap.h"
#include "guest_bridge_abi.h"

/* ole32 / oleaut32 bridges for the single guest process.
 *
 * Modelled (real, documented behaviour):
 *   - CoInitialize/CoInitializeEx: one guest thread, so the apartment is one
 *     counter plus the model that was requested (STA or MTA). The first call
 *     returns S_OK, a repeat with the same model S_FALSE, a conflicting model
 *     RPC_E_CHANGED_MODE. CoUninitialize decrements and never goes below zero.
 *   - CoTaskMemAlloc/Realloc/Free: real blocks on the port-owned guest process
 *     heap (runtime/guest_process_heap.h). No host pointer ever reaches the
 *     guest and no size header is invented.
 *   - CoCreateInstance: consults the in-process CLSID -> factory table that the
 *     host registered with gol_register_factory(). A CLSID that is not in that
 *     table is *not registered* and therefore returns the documented
 *     REGDB_E_CLASSNOTREG (0x80040154) with *ppv = NULL and a log line carrying
 *     both GUIDs. That is the documented answer for an unregistered class, not
 *     a fabricated object; nothing is ever synthesised for a known-but-missing
 *     COM server.
 *   - CoInitializeSecurity / CoSetProxyBlanket: S_OK. There is no DCOM, no
 *     remote endpoint and no authentication in this process, so the documented
 *     "no security" answer is the only truthful one. The arguments are not
 *     validated beyond the pointer ranges, which is a documented deviation.
 *   - CoFreeUnusedLibraries: no-op (VOID, nothing is loaded on demand).
 *   - SysAllocString/SysAllocStringLen/SysFreeString/SysStringLen: BSTR is the
 *     documented PE32 layout - a 4-byte byte-count prefix followed by the
 *     UTF-16 code units and a terminating NUL - allocated on the guest process
 *     heap. Live BSTRs are tracked so SysFreeString on a pointer this module did
 *     not hand out is a named stop instead of a wild free.
 *   - VariantClear: VT_EMPTY/VT_NULL and the scalar types are cleared in place,
 *     VT_BSTR frees the BSTR, VT_DISPATCH/VT_UNKNOWN call Release through the
 *     host-installed guest-vtable hook. By-ref and array variants are a named
 *     stop (no SAFEARRAY/VARIANT-by-ref model exists).
 *
 * Named stops (negative, never a Win32 status):
 *   OleLockRunning (no running-object table), OleCreateFontIndirect (no
 *   IFont/StdFont object), an unsupported VARIANT type, an unknown BSTR, a
 *   missing release hook and a bad argument count.
 *
 * Every entry point uses the shared bridge signature
 *   int fn(GuestOle *, const uint32_t *args, uint32_t *result)
 * with args[] in declaration order (args[0] = first stack argument) and
 * writes the Win32 return value to *result. A non-zero return is a model stop.
 */

#define GOL_S_OK 0x00000000u
#define GOL_S_FALSE 0x00000001u
#define GOL_E_INVALIDARG 0x80070057u
#define GOL_E_OUTOFMEMORY 0x8007000eu
#define GOL_E_NOINTERFACE 0x80004002u
#define GOL_E_FAIL 0x80004005u
#define GOL_E_UNEXPECTED 0x8000ffffu
#define GOL_RPC_E_CHANGED_MODE 0x80010106u
#define GOL_REGDB_E_CLASSNOTREG 0x80040154u
#define GOL_CLASS_E_NOAGGREGATION 0x80040110u

#define GOL_COINIT_MULTITHREADED 0x0u
#define GOL_COINIT_APARTMENTTHREADED 0x2u
#define GOL_COINIT_DISABLE_OLE1DDE 0x4u
#define GOL_COINIT_SPEED_OVER_MEMORY 0x8u
#define GOL_COINIT_KNOWN_MASK (GOL_COINIT_APARTMENTTHREADED | \
                               GOL_COINIT_DISABLE_OLE1DDE | \
                               GOL_COINIT_SPEED_OVER_MEMORY)

/* CLSCTX bits accepted by CoCreateInstance: in-process only. A remote or
 * out-of-process server cannot exist in this runtime. */
#define GOL_CLSCTX_INPROC_SERVER 0x1u
#define GOL_CLSCTX_INPROC_HANDLER 0x2u
#define GOL_CLSCTX_LOCAL_SERVER 0x4u
#define GOL_CLSCTX_REMOTE_SERVER 0x10u
#define GOL_CLSCTX_ALL 0x17u

/* VARIANT vt values used by VariantClear. */
#define GOL_VT_EMPTY 0u
#define GOL_VT_NULL 1u
#define GOL_VT_I2 2u
#define GOL_VT_I4 3u
#define GOL_VT_R4 4u
#define GOL_VT_R8 5u
#define GOL_VT_CY 6u
#define GOL_VT_DATE 7u
#define GOL_VT_BSTR 8u
#define GOL_VT_DISPATCH 9u
#define GOL_VT_ERROR 10u
#define GOL_VT_BOOL 11u
#define GOL_VT_VARIANT 12u
#define GOL_VT_UNKNOWN 13u
#define GOL_VT_DECIMAL 14u
#define GOL_VT_I1 16u
#define GOL_VT_UI1 17u
#define GOL_VT_UI2 18u
#define GOL_VT_UI4 19u
#define GOL_VT_I8 20u
#define GOL_VT_UI8 21u
#define GOL_VT_INT 22u
#define GOL_VT_UINT 23u
#define GOL_VT_ARRAY 0x2000u
#define GOL_VT_BYREF 0x4000u
#define GOL_VARIANT_SIZE 16u

/* Module-local named stops. */
enum {
    GU_STOP_OLE_LOCK_RUNNING = -2201,   /* no running-object table */
    GU_STOP_OLE_FONT_INDIRECT = -2202,  /* no IFont/StdFont object */
    GU_STOP_OLE_BAD_BSTR = -2203,       /* pointer was not issued here */
    GU_STOP_OLE_VARIANT_TYPE = -2204,   /* by-ref/array/unknown vt */
    GU_STOP_OLE_NO_RELEASE_HOOK = -2205,/* VT_DISPATCH without a guest call */
    GU_STOP_OLE_FACTORY = -2206,        /* factory callback failed */
    GU_STOP_OLE_BAD_ARGS = -2207,       /* argument outside the model */
    GU_STOP_OLE_NO_HEAP = -2208         /* guest process heap not available */
};

#define GOL_MAX_FACTORIES 16u
#define GOL_MAX_BSTR 256u
#define GOL_MAX_UNREGISTERED 16u

/* Factory: returns 0 and fills object/hresult, or a non-zero stop. */
typedef int (*GolFactory)(void *context, const uint8_t clsid[16],
                          const uint8_t iid[16], uint32_t outer,
                          uint32_t *object, uint32_t *hresult);
/* Guest vtable call for Release: reads the guest vtable, calls slot 2. */
typedef int (*GolGuestRelease)(void *context, uint32_t unknown,
                               uint32_t *hresult);

typedef struct GuestOle GuestOle;

/* The heap is borrowed and must stay alive; without it every allocation entry
 * point is a named stop instead of a host malloc. */
int gol_create(GuestMemory *, uint32_t teb, GuestProcessHeap *, GuestOle **out);
void gol_destroy(GuestOle *);
int gol_register_factory(GuestOle *, const uint8_t clsid[16], GolFactory,
                         void *context);
void gol_set_release_hook(GuestOle *, GolGuestRelease, void *context);

/* Diagnostics (host/tests only; no guest state). */
uint32_t gol_apartment_count(const GuestOle *);
uint32_t gol_apartment_model(const GuestOle *);
uint32_t gol_bstr_live(const GuestOle *);
uint32_t gol_unregistered(const GuestOle *);
int gol_unregistered_at(const GuestOle *, uint32_t index, uint8_t clsid[16],
                        uint8_t iid[16]);
const char *gol_last_stop(const GuestOle *);
const char *gol_stop_name(int code);

/* --- ole32 ------------------------------------------------------------- */
int gol_co_initialize(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_initialize_ex(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_uninitialize(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_task_mem_alloc(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_task_mem_realloc(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_task_mem_free(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_create_instance(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_co_initialize_security(GuestOle *, const uint32_t *args,
                               uint32_t *result);
int gol_co_set_proxy_blanket(GuestOle *, const uint32_t *args,
                             uint32_t *result);
int gol_co_free_unused_libraries(GuestOle *, const uint32_t *args,
                                 uint32_t *result);
int gol_ole_lock_running(GuestOle *, const uint32_t *args, uint32_t *result);

/* --- oleaut32 ---------------------------------------------------------- */
int gol_sys_alloc_string(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_sys_alloc_string_len(GuestOle *, const uint32_t *args,
                             uint32_t *result);
int gol_sys_free_string(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_sys_string_len(GuestOle *, const uint32_t *args, uint32_t *result);
int gol_variant_clear(GuestOle *, const uint32_t *args, uint32_t *result);
/* Host-side: a tracked BSTR holding an ASCII string (0 = out of memory). */
int gol_alloc_bstr_ascii(GuestOle *, const char *text, uint32_t *bstr);
int gol_ole_create_font_indirect(GuestOle *, const uint32_t *args,
                                 uint32_t *result);

/* Registration list for runtime/win32_api_table.c (wired later; that file is
 * not modified here). stdcall_arg_bytes is the number of stack bytes the callee
 * pops, i.e. the sum of the Win32 parameter sizes. */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestOle *, const uint32_t *args, uint32_t *result);
} GuestOleExport;

extern const GuestOleExport ole32_exports[];
extern const uint32_t ole32_export_count;
extern const GuestOleExport oleaut32_exports[];
extern const uint32_t oleaut32_export_count;

#endif
