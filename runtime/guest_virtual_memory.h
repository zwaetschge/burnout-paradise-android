#ifndef BURNOUT_GUEST_VIRTUAL_MEMORY_H
#define BURNOUT_GUEST_VIRTUAL_MEMORY_H
#include "guest_memory.h"
/* VirtualAlloc/VirtualFree/VirtualQuery over the port's GuestMemory.
 *
 * The managed address range is a constructor parameter (base..limit), so the
 * dynamic-allocation arena never collides with the static image, the module
 * pool or the process heap. Reservations are 64-KB allocation-granularity
 * aligned, commit/decommit work on 4-KB guest pages, and committed pages are
 * backed by zero-filled private host buffers that are only reachable through
 * gm_check/gm_read/gm_write. No host pointer is serialized into the guest.
 *
 * MEMORY_BASIC_INFORMATION is the PE32 28-byte layout:
 *   BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect,
 *   Type.
 *
 * Not modelled (named stop): MEM_RESET/MEM_PHYSICAL/MEM_LARGE_PAGES/MEM_TOP_DOWN
 * and other allocation types, PAGE_GUARD/PAGE_NOCACHE, executing committed
 * pages, and a length shorter than the 28-byte structure (the port requires the
 * full documented size instead of guessing a partial write). */
#define GVM_PAGE 0x1000u
#define GVM_GRANULARITY 0x10000u
#define GVM_MEM_COMMIT 0x00001000u
#define GVM_MEM_RESERVE 0x00002000u
#define GVM_MEM_DECOMMIT 0x00004000u
#define GVM_MEM_RELEASE 0x00008000u
#define GVM_MEM_FREE 0x00010000u
#define GVM_MEM_PRIVATE 0x00020000u
#define GVM_PAGE_NOACCESS 0x01u
#define GVM_MBI_SIZE 28u
#define GVM_ERROR_NOT_ENOUGH_MEMORY 8u
#define GVM_ERROR_INVALID_PARAMETER 87u
#define GVM_ERROR_INVALID_ADDRESS 487u

typedef struct {const char *dll;const char *name;uint32_t stdcall_arg_bytes;void *fn;} GvmExport;
extern const GvmExport gvm_exports[];
extern const uint32_t gvm_export_count;

typedef struct GuestVirtualMemory GuestVirtualMemory;
/* base must be allocation-granularity aligned, limit page aligned. */
GuestVirtualMemory *gvm_create(GuestMemory *memory,uint32_t base,uint32_t limit);
void gvm_destroy(GuestVirtualMemory *vm);
int gvm_alloc(GuestVirtualMemory *,uint32_t address,uint32_t size,uint32_t type,
              uint32_t protect,uint32_t *result);
int gvm_free(GuestVirtualMemory *,uint32_t address,uint32_t size,uint32_t type,uint32_t *result);
int gvm_query(GuestVirtualMemory *,uint32_t address,uint32_t buffer,uint32_t length,
              uint32_t *result);
/* Diagnostics: {base,size,protect,committed_bytes} per reservation and
 * {start,size,protect,owner} per committed range. Returns the total count. */
uint32_t gvm_regions(GuestVirtualMemory *,uint32_t *quads,uint32_t capacity);
uint32_t gvm_commits(GuestVirtualMemory *,uint32_t *quads,uint32_t capacity);
#endif
