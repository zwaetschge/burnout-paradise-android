#ifndef BURNOUT_GUEST_PROCESS_HEAP_H
#define BURNOUT_GUEST_PROCESS_HEAP_H
#include "guest_memory.h"
#define GPH_HANDLE 0x78000000u
#define GPH_BASE 0x11000000u
#define GPH_LIMIT 0x20000000u
typedef struct GuestProcessHeap GuestProcessHeap;
/* Port-owned opaque handle, single guest CPU. No Windows heap internals,
 * exceptions, private heaps or concurrent guest allocators are implied. */
GuestProcessHeap *gph_create(GuestMemory *memory);
void gph_destroy(GuestProcessHeap *heap);
int gph_initialize(GuestProcessHeap *heap,uint32_t peb);
int gph_current(GuestProcessHeap *heap,uint32_t peb,uint32_t *handle);
int gph_alloc(GuestProcessHeap *heap,uint32_t handle,uint32_t flags,uint32_t size,uint32_t *address);
int gph_free(GuestProcessHeap *heap,uint32_t handle,uint32_t flags,uint32_t address);
int gph_size(GuestProcessHeap *heap,uint32_t handle,uint32_t flags,uint32_t address,uint32_t *size);
int gph_realloc(GuestProcessHeap *heap,uint32_t handle,uint32_t flags,uint32_t address,uint32_t size,uint32_t *result);
/* Triples: address, mapped capacity, requested size. Backing remains private. */
uint32_t gph_regions(GuestProcessHeap *heap,uint32_t *triples,uint32_t capacity);
#endif
