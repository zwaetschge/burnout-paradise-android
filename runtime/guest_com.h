#ifndef BURNOUT_GUEST_COM_H
#define BURNOUT_GUEST_COM_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"

/* Generic 32-bit COM framework for the D3D11/DXGI guest objects.
 *
 * Guest objects live in guest memory: an object is a 16-byte block whose first
 * dword is the guest address of its vtable. The vtable itself is an array of
 * guest addresses inside the reserved trap range; calling one of those
 * addresses transfers control to the host resolver, which maps the address
 * back to (interface, slot) and either executes the real IUnknown method or
 * reports a named stop for the unimplemented method.
 *
 * Only IUnknown is implemented for real:
 *   slot 0 QueryInterface(this, riid, ppv)  -> S_OK / E_NOINTERFACE
 *   slot 1 AddRef(this)                     -> new reference count
 *   slot 2 Release(this)                    -> remaining reference count
 * Every other slot is a named stop of the form
 *   d3d11-unimplemented: <Interface>::<Method>
 * and never returns a fabricated HRESULT.
 *
 * No host pointer is ever written into guest memory: objects, vtables and
 * GUIDs are all guest addresses. */
#define GCOM_TRAP_BASE 0x6f000000u
#define GCOM_TRAP_STRIDE 16u
#define GCOM_TRAP_LIMIT 0x6f800000u
#define GCOM_OBJECT_SIZE 16u
#define GCOM_MAGIC 0x314d4f43u /* "COM1" */

#define GCOM_S_OK 0x00000000u
#define GCOM_E_NOINTERFACE 0x80004002u
#define GCOM_E_INVALIDARG 0x80070057u
#define GCOM_E_OUTOFMEMORY 0x8007000eu

typedef struct {
    const char *name;
    uint32_t arg_bytes; /* stdcall bytes including `this` (4 + 4*args) */
} GuComMethod;

typedef struct {
    const char *name;
    uint8_t iid[16];   /* documented interface id, all zero when unknown */
    int iid_known;
    const GuComMethod *methods;
    uint32_t method_count;
} GuComInterface;

/* Interface tables (runtime/guest_d3d11_vtables.c). */
extern const GuComInterface gu_com_interfaces[];
extern const uint32_t gu_com_interface_count;

typedef struct GuestCom GuestCom;

GuestCom *gcom_create(GuestMemory *memory, uint32_t teb);
void gcom_destroy(GuestCom *com);
/* Guest arena for vtables/objects (host-mapped, writable). */
int gcom_set_arena(GuestCom *com, uint32_t base, uint32_t size);

/* Interface table access (runtime/guest_d3d11_vtables.c). */
uint32_t gcom_interface_count(void);
const GuComInterface *gcom_interface_at(uint32_t index);
/* -1 when the name is not in the table. */
int gcom_interface_index(const char *name);
uint32_t gcom_total_methods(void);
/* Slot count and stdcall argument bytes (including `this`) of one method. */
uint32_t gcom_method_count(uint32_t interface_index);
uint32_t gcom_method_arg_bytes(uint32_t interface_index, uint32_t slot);
/* Guest trap address of one (interface, slot); 0 when out of range. */
uint32_t gcom_trap_address(uint32_t interface_index, uint32_t slot);
/* Resolves a guest trap address back to (interface, slot). */
int gcom_resolve(GuestCom *com, uint32_t address, uint32_t *interface_index,
                 uint32_t *slot);

/* Creates a guest object implementing `interface_index` (vtable + object in
 * the arena) and returns its guest address in *object. */
int gcom_create_object(GuestCom *com, uint32_t interface_index, uint32_t *object);
/* Guest address of the vtable of a created object. */
int gcom_object_vtable(GuestCom *com, uint32_t object, uint32_t *vtable);
uint32_t gcom_live_objects(GuestCom *com);
uint32_t gcom_refcount(GuestCom *com, uint32_t object);

/* Executes the method in `slot` of `object`. args[] holds the stdcall
 * arguments in declaration order (args[0] = this). *result receives the
 * HRESULT/ULONG for IUnknown, untouched for a named stop. */
int gcom_call(GuestCom *com, uint32_t object, uint32_t slot,
              const uint32_t *args, uint32_t *result);
/* Same, but resolved from a guest trap address (the vtable entry value). */
int gcom_call_trap(GuestCom *com, uint32_t trap_address, const uint32_t *args,
                   uint32_t *result);

/* Diagnostics: last named stop ("Interface::Method") and its slot. */
const char *gcom_last_stop(GuestCom *com);
uint32_t gcom_last_stop_slot(GuestCom *com);
const char *gcom_stop_interface(GuestCom *com);

/* --- D3D11/DXGI entry points ------------------------------------------ */
int gcom_d3d11_create_device(GuestCom *, const uint32_t *args, uint32_t *result);
int gcom_d3d11_create_device_and_swap_chain(GuestCom *, const uint32_t *args,
                                            uint32_t *result);
int gcom_create_dxgi_factory(GuestCom *, const uint32_t *args, uint32_t *result);
int gcom_create_dxgi_factory1(GuestCom *, const uint32_t *args, uint32_t *result);

typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestCom *, const uint32_t *args, uint32_t *result);
} GuComExport;

extern const GuComExport gu_com_exports[];
extern const uint32_t gu_com_export_count;

#endif
