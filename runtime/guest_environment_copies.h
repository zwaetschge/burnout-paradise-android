#ifndef BURNOUT_GUEST_ENVIRONMENT_COPIES_H
#define BURNOUT_GUEST_ENVIRONMENT_COPIES_H
#include "guest_process_heap.h"
typedef struct GuestEnvironmentCopies GuestEnvironmentCopies;
GuestEnvironmentCopies *gec_create(GuestMemory *,GuestProcessHeap *,uint32_t source);
/* Frees ownership records; the process heap retains/frees its own blocks. */
void gec_destroy(GuestEnvironmentCopies *);
int gec_get_w(GuestEnvironmentCopies *,uint32_t *address);
int gec_free_w(GuestEnvironmentCopies *,uint32_t address);
uint32_t gec_blocks(const GuestEnvironmentCopies *,uint32_t *addresses,uint32_t capacity);
#endif
