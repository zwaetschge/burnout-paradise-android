#ifndef BURNOUT_SHIM_LOADER_H
#define BURNOUT_SHIM_LOADER_H
#include <stdint.h>
#include <stddef.h>
typedef struct { const char *name; uint32_t handle; } ShimModule;
typedef struct { uint32_t module; const char *name; uint32_t ordinal,target; } ShimExport;
typedef struct {
    const ShimModule *modules; size_t module_count;
    const ShimExport *exports; size_t export_count;
    uint32_t *references;
} ShimLoader;
/* Registered host-shim modules only. No arbitrary DLL file loading or PE
 * introspection. Missing entries stop the runtime, not fake a loaded DLL. */
int sl_load(ShimLoader *s,const char *name,uint32_t *handle);
int sl_symbol(ShimLoader *s,uint32_t handle,const char *name,uint32_t ordinal,uint32_t *target);
/* Read-only loaded-module lookup; a known/ordinary absent basename returns0. */
int sl_get_loaded(const ShimLoader *,const char *name,const char *main_name,uint32_t main_image,uint32_t *handle);
#endif
