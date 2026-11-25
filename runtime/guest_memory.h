#ifndef BURNOUT_GUEST_MEMORY_H
#define BURNOUT_GUEST_MEMORY_H
#include <stdint.h>
#include <stddef.h>

/* Guest pages are 4 KB even on hosts with 16 KB pages. These permissions
 * govern data accesses through translated helpers, not native host mprotect. */
enum { GM_READ=1, GM_WRITE=2, GM_EXECUTE=4, GM_PAGE=4096 };
enum { GM_OK=0, GM_RANGE=1, GM_UNCOMMITTED=2, GM_ALLOCATION=3,
       GM_ACCESS=4, GM_UNSUPPORTED=5, GM_OVERLAP=6 };
typedef struct GuestMemory GuestMemory;
GuestMemory *gm_create(void);
void gm_destroy(GuestMemory *m);
/* Caller owns backing bytes. NULL backing reserves without committing. */
int gm_map(GuestMemory *m,uint32_t address,uint32_t size,void *backing,
           uint32_t allocation,uint32_t protection);
int gm_unmap(GuestMemory *m,uint32_t address,uint32_t size,uint32_t allocation);
/* Prototype shared-system pages expose only implemented fields. The caller
 * owns this 512-byte bitmap for a committed read-only 4-KB page. Unimplemented
 * bytes return GM_UNSUPPORTED; the page cannot be made writable/executable. */
int gm_limit_readable_bytes(GuestMemory *,uint32_t page,const uint8_t *bitmap);
int gm_check(GuestMemory *m,uint32_t address,uint32_t size,uint32_t access);
int gm_read(GuestMemory *m,uint32_t address,void *out,uint32_t size);
int gm_fetch(GuestMemory *m,uint32_t address,void *out,uint32_t size);
int gm_write(GuestMemory *m,uint32_t address,const void *in,uint32_t size);
uint32_t gm_protection(GuestMemory *m,uint32_t address);
int gm_protect(GuestMemory *m,uint32_t address,uint32_t size,uint32_t protection,
               uint32_t *old_protection);
/* Subset: ordinary protection on committed pages; COW/guard/cache/CFG flags
 * return GM_UNSUPPORTED and must stop the caller, not pretend Win32 success. */
int gm_virtual_protect(GuestMemory *m,uint32_t address,uint32_t size,
                       uint32_t protection,uint32_t old_protection_guest);
uint32_t gm_access_bits(uint32_t protection);
#endif
