#ifndef BURNOUT_GUEST_WBEM_H
#define BURNOUT_GUEST_WBEM_H
#include <stdint.h>
#include "guest_memory.h"

/* WMI (wbemprox) guest COM objects: the part of the CIM repository this port
 * has.
 *
 * Why it exists: the original image only polls XInput for a controller that
 * (1) DirectInput enumerates and (2) WMI lists as a Win32_PNPEntity whose
 * DeviceID carries "IG_" and the same VID/PID as the DirectInput product GUID
 * (the documented "is this DirectInput device an XInput device" test, image
 * function 0x007EA1A0). So the platform gamepad has to exist in both views.
 *
 * Modelled (documented behaviour):
 *   IWbemLocator::ConnectServer        namespace root\cimv2 (with or without
 *                                      the \\.\ prefix); any other namespace is
 *                                      WBEM_E_INVALID_NAMESPACE
 *   IWbemServices::CreateInstanceEnum  class Win32_PNPEntity: the entities set
 *                                      with gwb_set_pnp_entities(); any other
 *                                      class is WBEM_E_INVALID_CLASS
 *   IWbemServices::ExecQuery           WBEM_E_NOT_SUPPORTED (there is no WQL
 *                                      engine; *ppEnum = NULL)
 *   IEnumWbemClassObject::Next/Reset   S_OK / WBEM_S_FALSE as documented
 *   IWbemClassObject::Get              "DeviceID" (VT_BSTR, CIM_STRING); any
 *                                      other property is WBEM_E_NOT_FOUND
 *   IUnknown on all four interfaces
 * Every other method is a named stop.
 *
 * Guest layout: four vtables and up to GWB_MAX_OBJECTS 16-byte objects in the
 * arena given to gwb_set_arena(); the vtable entries are trap addresses in
 * GWB_TRAP_BASE..GWB_TRAP_LIMIT (never mapped as code). */
#define GWB_TRAP_BASE 0x6e3f0000u
#define GWB_TRAP_LIMIT 0x6e400000u
#define GWB_TRAP_STRIDE 16u
#define GWB_SLOTS_PER_INTERFACE 32u
#define GWB_MAX_OBJECTS 64u
#define GWB_MAX_ENTITIES 8u
#define GWB_ARENA_MIN 0x1000u

#define GWB_S_OK 0x00000000u
#define GWB_S_FALSE 0x00000001u
#define GWB_E_NOINTERFACE 0x80004002u
#define GWB_E_NOT_FOUND 0x80041002u
#define GWB_E_INVALID_PARAMETER 0x80041008u
#define GWB_E_NOT_SUPPORTED 0x8004100cu
#define GWB_E_INVALID_NAMESPACE 0x8004100eu
#define GWB_E_INVALID_CLASS 0x80041010u
#define GWB_E_OUT_OF_MEMORY 0x80041006u

enum {
    GU_STOP_WBEM_UNIMPLEMENTED = -2901,
    GU_STOP_WBEM_BAD_OBJECT = -2902,
    GU_STOP_WBEM_NO_ARENA = -2903,
    GU_STOP_WBEM_BAD_ARGS = -2904,
    GU_STOP_WBEM_NO_BSTR = -2905
};

typedef struct GuestWbem GuestWbem;
/* Allocates a BSTR holding `ascii` on the calling guest thread's OLE heap. */
typedef int (*GwbBstrAlloc)(void *context, const char *ascii, uint32_t *bstr);

int gwb_create(GuestMemory *, GuestWbem **out);
void gwb_destroy(GuestWbem *);
int gwb_set_arena(GuestWbem *, uint32_t base, uint32_t size);
void gwb_set_bstr_alloc(GuestWbem *, GwbBstrAlloc, void *context);
/* The Win32_PNPEntity instances (their DeviceID strings, copied). */
int gwb_set_pnp_entities(GuestWbem *, const char *const *device_ids, uint32_t count);

/* CoCreateInstance factory for CLSID_WbemLocator (GolFactory signature). */
extern const uint8_t gwb_clsid_locator[16];
int gwb_factory(void *context, const uint8_t clsid[16], const uint8_t iid[16],
                uint32_t outer, uint32_t *object, uint32_t *hresult);

static inline int gwb_is_trap(uint32_t va) { return va >= GWB_TRAP_BASE && va < GWB_TRAP_LIMIT; }
/* Calls the method behind `trap` (args[0] = this). Returns the stdcall bytes to
 * pop (>= 4) or a negative named stop; `*what` names the method. */
int gwb_call(GuestWbem *, uint32_t trap, const uint32_t *args, uint32_t *result, const char **what);
uint32_t gwb_live_objects(const GuestWbem *);

#endif
