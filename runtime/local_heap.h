#ifndef BURNOUT_LOCAL_HEAP_H
#define BURNOUT_LOCAL_HEAP_H
#include "guest_memory.h"
typedef struct GuestLocalHeap GuestLocalHeap;
/* Fixed local blocks, serialized by the caller. No movable-handle API. */
GuestLocalHeap *lh_create(GuestMemory *memory);
void lh_destroy(GuestLocalHeap *heap);
int lh_alloc(GuestLocalHeap *heap,uint32_t size,uint32_t *address);
int lh_free(GuestLocalHeap *heap,uint32_t address);
uint32_t lh_regions(GuestLocalHeap *heap,uint32_t *address_size_pairs,uint32_t capacity);
#endif
