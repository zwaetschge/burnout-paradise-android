#include "guest_static_tls.h"
#include <stdlib.h>
#include <string.h>
static int write32(GuestMemory *m,uint32_t a,uint32_t value){
    uint8_t bytes[4];for(unsigned i=0;i<4;i++)bytes[i]=(uint8_t)(value>>(8*i));
    return gm_write(m,a,bytes,4);
}
int gsti_initialize(GuestMemory *m,uint32_t teb,uint32_t vector,uint32_t slots){
    if(!m || !slots || slots>1024 || (vector&3) || (uint64_t)teb+0x30>UINT64_C(0x100000000))return GM_RANGE;
    int rc=gm_check(m,vector,slots*4,GM_WRITE);if(rc)return rc;
    rc=gm_check(m,teb+0x2c,4,GM_WRITE);if(rc)return rc;
    uint8_t zeros[4096]={0};rc=gm_write(m,vector,zeros,slots*4);
    return rc ? rc : write32(m,teb+0x2c,vector);
}
int gsti_attach(GuestMemory *m,uint32_t vector,uint32_t slots,uint32_t slot,
                uint32_t index_address,uint32_t storage,uint32_t capacity,
                const void *template_bytes,uint32_t template_size,
                uint32_t zero_fill,uint32_t alignment){
    if(!m || !slots || slots>1024 || slot>=slots || (uint64_t)vector+slots*4>UINT64_C(0x100000000) ||
       !capacity || capacity>16u*1024*1024 || (uint64_t)template_size+zero_fill>capacity ||
       (template_size && !template_bytes) || !alignment || (alignment&(alignment-1)) || (storage&(alignment-1)))return GM_RANGE;
    uint32_t cell=vector+slot*4;uint8_t previous[4];
    int rc=gm_check(m,cell,4,GM_READ|GM_WRITE);if(rc)return rc;
    rc=gm_check(m,index_address,4,GM_WRITE);if(rc)return rc;
    rc=gm_check(m,storage,capacity,GM_WRITE);if(rc)return rc;
    rc=gm_read(m,cell,previous,4);if(rc)return rc;
    if(previous[0] || previous[1] || previous[2] || previous[3])return GM_OVERLAP;
    uint8_t *bytes=calloc(1,capacity);if(!bytes)return GM_RANGE;
    if(template_size)memcpy(bytes,template_bytes,template_size);
    rc=gm_write(m,storage,bytes,capacity);free(bytes);if(rc)return rc;
    rc=write32(m,index_address,slot);return rc ? rc : write32(m,cell,storage);
}
