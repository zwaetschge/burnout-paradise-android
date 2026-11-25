#ifndef BURNOUT_GUEST_PEB_H
#define BURNOUT_GUEST_PEB_H
#include "guest_memory.h"
#include "shim_loader.h"
#define GP_BASE 0x75000000u
#define GP_SIZE 0x10000u
/* PE32 serialized process metadata for our registered host-shim loader.
 * No host pointers, PE DLL execution, debugger/entitlement state or OS version
 * fabrication. Optional mapped file views provide explicit size/entry metadata;
 * other shim bases remain opaque handles.
 * Unmodeled PEB/RTL process fields are not a Windows conformance claim. */
int gp_initialize(GuestMemory *,uint32_t image,uint32_t size,uint32_t entry,
                  uint32_t command,uint32_t command_bytes,const ShimLoader *);
int gp_sync_modules(GuestMemory *,uint32_t image,uint32_t size,uint32_t entry,const ShimLoader *);
typedef struct {uint32_t base,size,entry;} GuestModuleImage;
int gp_sync_image_views(GuestMemory *,const ShimLoader *,const GuestModuleImage *,uint32_t count);
int gp_system_fields(GuestMemory *,uint32_t major,uint32_t minor,uint32_t build,
                     uint32_t processors,uint32_t subsystem,uint32_t subsystem_major,uint32_t subsystem_minor);
/* Runtime-owned guest IDs, serialized in the PE32 TEB's ClientId fields. */
int gp_thread_identity(GuestMemory *,uint32_t teb,uint32_t process_id,uint32_t thread_id);
int gp_current_pseudo(uint32_t thread,uint32_t *value);
int gp_current_identity(GuestMemory *,uint32_t teb,uint32_t thread,uint32_t *value);
/* GetModuleFileNameA subset: current main EXE from the loaded-module metadata. */
int gp_main_file_name_a(GuestMemory *,uint32_t peb,uint32_t module,uint32_t output,uint32_t capacity,uint32_t *result,uint32_t *error);
#endif
