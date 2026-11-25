#ifndef BURNOUT_SHIM_LOADER_WIDE_H
#define BURNOUT_SHIM_LOADER_WIDE_H
#include "shim_loader.h"
#include "guest_memory.h"
#include "guest_apiset.h"
/* Reached subset: reserved file=NULL, SEARCH_SYSTEM32, ASCII system/API-set
 * names encoded as UTF16. Registered native shims only; not guest PE DllMain. */
int sl_load_exw(ShimLoader *,GuestMemory *,const GuestApiSet *,size_t aliases,
                uint32_t name,uint32_t reserved_file,uint32_t flags,uint32_t *handle);
int sl_get_module_w(const ShimLoader *,GuestMemory *,uint32_t name,const char *main_name,uint32_t main_image,uint32_t *handle);
#endif
