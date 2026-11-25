#ifndef BURNOUT_GUEST_STATIC_TLS_H
#define BURNOUT_GUEST_STATIC_TLS_H
#include "guest_memory.h"
/* One guest thread. Backing mappings and unique module slots are owned by the
 * loader. This is separate from TlsAlloc/TlsGetValue's dynamic TLS slots.
 * Callers must execute any TLS callbacks and DllMain after attaching storage. */
int gsti_initialize(GuestMemory *,uint32_t teb,uint32_t vector,uint32_t slots);
int gsti_attach(GuestMemory *,uint32_t vector,uint32_t slots,uint32_t slot,
                uint32_t index_address,uint32_t storage,uint32_t capacity,
                const void *template_bytes,uint32_t template_size,
                uint32_t zero_fill,uint32_t alignment);
#endif
