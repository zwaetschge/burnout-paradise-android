#include "guest_memory.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *bytes; uint32_t allocation, protection; const uint8_t *read_bitmap; } Page;
struct GuestMemory { Page pages[1u<<20]; };

GuestMemory *gm_create(void) { return calloc(1,sizeof(GuestMemory)); }
void gm_destroy(GuestMemory *m) { free(m); }
uint32_t gm_access_bits(uint32_t p) {
    switch(p) {
    case 1: return 0; case 2: return GM_READ;
    case 4: return GM_READ|GM_WRITE; case 0x10: return GM_EXECUTE;
    case 0x20: return GM_READ|GM_EXECUTE;
    case 0x40: return GM_READ|GM_WRITE|GM_EXECUTE;
    default: return UINT32_MAX;
    }
}
static int bounds(uint32_t a,uint32_t n,uint32_t *first,uint32_t *last) {
    uint64_t end=(uint64_t)a+n;
    if (!n || end>0x100000000ull) return GM_RANGE;
    *first=a/GM_PAGE; *last=(uint32_t)((end-1)/GM_PAGE); return GM_OK;
}
int gm_map(GuestMemory *m,uint32_t a,uint32_t n,void *bytes,uint32_t owner,uint32_t p) {
    uint32_t first,last;
    if (!m || !owner || (a|n)&(GM_PAGE-1) || bounds(a,n,&first,&last)) return GM_RANGE;
    if (gm_access_bits(p)==UINT32_MAX) return GM_UNSUPPORTED;
    for (uint32_t i=first;i<=last;i++) if (m->pages[i].allocation) return GM_OVERLAP;
    for (uint32_t i=first;i<=last;i++)
        m->pages[i]=(Page){bytes ? (uint8_t*)bytes+(size_t)(i-first)*GM_PAGE : NULL,owner,p,NULL};
    return GM_OK;
}
int gm_check(GuestMemory *m,uint32_t a,uint32_t n,uint32_t access) {
    uint32_t first,last;
    if (!m || bounds(a,n,&first,&last)) return GM_RANGE;
    for (uint32_t i=first;i<=last;i++) {
        Page *p=&m->pages[i];
        if (!p->bytes) return GM_UNCOMMITTED;
        if ((gm_access_bits(p->protection)&access)!=access) return GM_ACCESS;
        if((access&GM_READ) && p->read_bitmap){
            uint32_t lo=i==first ? a&(GM_PAGE-1) : 0;
            uint32_t hi=i==last ? (uint32_t)(((uint64_t)a+n-1)&(GM_PAGE-1))+1 : GM_PAGE;
            for(uint32_t k=lo;k<hi;k++)if(!(p->read_bitmap[k>>3]&(1u<<(k&7))))return GM_UNSUPPORTED;
        }
    }
    return GM_OK;
}
int gm_limit_readable_bytes(GuestMemory *m,uint32_t page,const uint8_t *bitmap){
    if(!m || !bitmap || (page&(GM_PAGE-1)))return GM_RANGE;
    Page *p=&m->pages[page/GM_PAGE];
    if(!p->bytes)return GM_UNCOMMITTED;
    if(p->protection!=2)return GM_UNSUPPORTED;
    p->read_bitmap=bitmap;return GM_OK;
}
int gm_unmap(GuestMemory *m,uint32_t a,uint32_t n,uint32_t owner) {
    uint32_t first,last;
    if(!m || !owner || (a|n)&(GM_PAGE-1) || bounds(a,n,&first,&last))return GM_RANGE;
    for(uint32_t i=first;i<=last;i++)if(m->pages[i].allocation!=owner)return GM_ALLOCATION;
    for(uint32_t i=first;i<=last;i++)m->pages[i]=(Page){0};
    return GM_OK;
}
static void transfer(GuestMemory *m,uint32_t a,void *buffer,uint32_t n,int write) {
    uint8_t *b=buffer;
    while (n) {
        uint32_t offset=a&(GM_PAGE-1), amount=GM_PAGE-offset;
        if (amount>n) amount=n;
        uint8_t *host=m->pages[a/GM_PAGE].bytes+offset;
        if (write) memcpy(host,b,amount); else memcpy(b,host,amount);
        a+=amount; b+=amount; n-=amount;
    }
}
int gm_read(GuestMemory *m,uint32_t a,void *out,uint32_t n) {
    int rc=gm_check(m,a,n,GM_READ); if (!rc) transfer(m,a,out,n,0); return rc;
}
int gm_fetch(GuestMemory *m,uint32_t a,void *out,uint32_t n) {
    int rc=gm_check(m,a,n,GM_EXECUTE); if (!rc) transfer(m,a,out,n,0); return rc;
}
int gm_write(GuestMemory *m,uint32_t a,const void *in,uint32_t n) {
    int rc=gm_check(m,a,n,GM_WRITE); if (!rc) transfer(m,a,(void*)in,n,1); return rc;
}
uint32_t gm_protection(GuestMemory *m,uint32_t a) { return m->pages[a/GM_PAGE].protection; }
static int validate_protect(GuestMemory *m,uint32_t a,uint32_t n,uint32_t p,
                             uint32_t *first,uint32_t *last) {
    if (!m || bounds(a,n,first,last)) return GM_RANGE;
    if (gm_access_bits(p)==UINT32_MAX) return GM_UNSUPPORTED;
    uint32_t owner=m->pages[*first].allocation;
    for (uint32_t i=*first;i<=*last;i++) {
        if (!m->pages[i].bytes) return GM_UNCOMMITTED;
        if (m->pages[i].allocation!=owner) return GM_ALLOCATION;
        if(m->pages[i].read_bitmap && p!=m->pages[i].protection)return GM_UNSUPPORTED;
    }
    return GM_OK;
}
int gm_protect(GuestMemory *m,uint32_t a,uint32_t n,uint32_t p,uint32_t *old) {
    uint32_t first,last; int rc=validate_protect(m,a,n,p,&first,&last); if (rc) return rc;
    if (old) *old=m->pages[first].protection;
    for (uint32_t i=first;i<=last;i++) m->pages[i].protection=p;
    return GM_OK;
}
int gm_virtual_protect(GuestMemory *m,uint32_t a,uint32_t n,uint32_t p,uint32_t out) {
    uint32_t first,last; int rc=validate_protect(m,a,n,p,&first,&last); if (rc) return rc;
    if (!out) return GM_ACCESS;
    rc=gm_check(m,out,4,GM_WRITE); if (rc) return rc;
    if (!(gm_access_bits(p)&GM_WRITE) && (uint64_t)out+4>(uint64_t)first*GM_PAGE &&
        out<((uint64_t)last+1)*GM_PAGE) return GM_UNSUPPORTED;
    uint32_t old=m->pages[first].protection;
    /* Serialize little-endian explicitly; do not expose native pointer widths. */
    uint8_t data[4]={(uint8_t)old,(uint8_t)(old>>8),(uint8_t)(old>>16),(uint8_t)(old>>24)};
    transfer(m,out,data,4,1);
    for (uint32_t i=first;i<=last;i++) m->pages[i].protection=p;
    return GM_OK;
}
