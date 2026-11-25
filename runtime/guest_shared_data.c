#include "guest_shared_data.h"
#include "guest_memory.h"
#include "guest_system_time.h"
#include <string.h>
#include <unistd.h>
int gs_version_fields(uint8_t *page,uint8_t *bitmap,uint32_t major,uint32_t minor,uint32_t build){
    if(!page || !bitmap)return GM_RANGE;
    memset(page,0,GS_SIZE);memset(bitmap,0,GS_MASK_SIZE);
    const uint32_t offsets[]={0x260,0x26c,0x270},values[]={build,major,minor};
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++){
        uint32_t k=offsets[i]+j;page[k]=(uint8_t)(values[i]>>(j*8));bitmap[k>>3]|=(uint8_t)(1u<<(k&7));
    }
    return 0;
}
int gs_haswell_legacy_features(uint8_t *page,uint8_t *bitmap,const uint32_t leaf1[4]){
    if(!page || !bitmap || !leaf1)return GM_RANGE;
    if((leaf1[0]&0xffff0u)!=0x306c0u)return GM_UNSUPPORTED;
    /* Microsoft PF0: legacy Pentium divide erratum, absent in this model.
     * PF1: no FPU; PF2: CX8; PF3: MMX. These describe the guest, not ARM. */
    /* PF4/5 describe PPC/Alpha and PF7 is 3DNow, absent on Haswell.
     * PF11 DAZ is captured from this pinned model's FXSAVE MXCSR_MASK=ffff;
     * see diagnostic-cpu-state.json. This is not inferred from the ARM host. */
    const uint8_t values[]={0,(uint8_t)!(leaf1[3]&1u),(uint8_t)((leaf1[3]>>8)&1u),
        (uint8_t)((leaf1[3]>>23)&1u),0,0,
        (uint8_t)(((leaf1[3]>>24)&1u)&&((leaf1[3]>>25)&1u)),0,
        (uint8_t)((leaf1[3]>>4)&1u),(uint8_t)((leaf1[3]>>6)&1u),
        (uint8_t)((leaf1[3]>>26)&1u),(uint8_t)((leaf1[3]>>26)&1u)};
    for(unsigned i=0;i<sizeof(values);i++){uint32_t k=0x274+i;page[k]=values[i];bitmap[k>>3]|=(uint8_t)(1u<<(k&7));}
    return 0;
}
int gs_haswell_nx_features(uint8_t *page,uint8_t *bitmap,const uint32_t leaf1[4],const uint32_t extended1[4]){
    if(!page || !bitmap || !leaf1 || !extended1)return GM_RANGE;
    if((leaf1[0]&0xffff0u)!=0x306c0u)return GM_UNSUPPORTED;
    /* The runtime checks execute permission independently of read access.
     * PF15 names Itanium CMP8XCHG16, not x86 CMPXCHG16B (PF14). */
    const uint8_t values[]={(uint8_t)((extended1[3]>>20)&1u),
        (uint8_t)(leaf1[2]&1u),(uint8_t)((leaf1[2]>>13)&1u),0};
    for(unsigned i=0;i<sizeof(values);i++){uint32_t k=0x280+i;page[k]=values[i];bitmap[k>>3]|=(uint8_t)(1u<<(k&7));}
    return 0;
}
int gs_haswell_xstate_features(uint8_t *page,uint8_t *bitmap,const uint32_t leaf1[4]){
    if(!page || !bitmap || !leaf1)return GM_RANGE;
    if((leaf1[0]&0xffff0u)!=0x306c0u)return GM_UNSUPPORTED;
    /* Explicit prototype capability policy: processor channels are not
     * enabled. This is no claim about physical motherboard memory channels.
     * The pinned diagnostic CPU has XCR0=3 (x87/SSE), not AVX state. */
    const uint8_t values[]={0,(uint8_t)(((leaf1[2]>>26)&1u)&&((leaf1[2]>>27)&1u)),0,0};
    for(unsigned i=0;i<sizeof(values);i++){uint32_t k=0x284+i;page[k]=values[i];bitmap[k>>3]|=(uint8_t)(1u<<(k&7));}
    return 0;
}
int gs_host_physical_bytes(uint64_t *bytes){
    if(!bytes)return GM_RANGE;
    long pages=sysconf(_SC_PHYS_PAGES),size=sysconf(_SC_PAGESIZE);
    if(pages<=0 || size<=0 || (uint64_t)pages>UINT64_MAX/(uint64_t)size)return GM_RANGE;
    *bytes=(uint64_t)pages*(uint64_t)size;return 0;
}
int gs_physical_memory_field(uint8_t *page,uint8_t *bitmap,uint64_t bytes){
    if(!page || !bitmap || !bytes || (bytes&4095u) || bytes/4096u>UINT32_MAX)return GM_RANGE;
    uint32_t count=(uint32_t)(bytes/4096u);
    for(unsigned i=0;i<4;i++){uint32_t k=0x2e8+i;page[k]=(uint8_t)(count>>(i*8));bitmap[k>>3]|=(uint8_t)(1u<<(k&7));}
    return 0;
}
int gs_system_root_field(uint8_t *page,uint8_t *bitmap,const char *root){
    if(!page || !bitmap || !root)return GM_RANGE;
    uint8_t utf16[520]={0};unsigned n=0;
    for(;n<260 && root[n];n++){
        if((uint8_t)root[n]>=128u)return GM_UNSUPPORTED;
        utf16[n*2]=(uint8_t)root[n];
    }
    if(!n || n==260)return GM_RANGE;
    memcpy(page+0x30,utf16,sizeof(utf16));
    for(unsigned k=0x30;k<0x238;k++)bitmap[k>>3]|=(uint8_t)(1u<<(k&7));
    return 0;
}
int gs_system_time_field(uint8_t *page,uint8_t *bitmap,uint64_t filetime){
    if(!page || !bitmap)return GM_RANGE;
    gst_store(page,filetime);
    for(unsigned k=GST_OFFSET;k<GST_OFFSET+GST_SIZE;k++)bitmap[k>>3]|=(uint8_t)(1u<<(k&7));
    return 0;
}
int gs_timezone_field(uint8_t *page,uint8_t *bitmap,int64_t bias){
    if(!page || !bitmap)return GM_RANGE;
    gst_store_bias(page,bias);
    for(unsigned k=GST_ZONE_OFFSET;k<GST_ZONE_OFFSET+GST_SIZE;k++)bitmap[k>>3]|=(uint8_t)(1u<<(k&7));
    return 0;
}
int gs_interrupt_time_field(uint8_t *page,uint8_t *bitmap,uint64_t ticks){
    if(!page || !bitmap)return GM_RANGE;
    gst_store_interrupt(page,ticks);
    for(unsigned k=GST_INTERRUPT_OFFSET;k<GST_INTERRUPT_OFFSET+GST_SIZE;k++)bitmap[k>>3]|=(uint8_t)(1u<<(k&7));
    return GM_OK;
}
int gs_processor_feature(GuestMemory *memory,uint32_t feature,uint32_t *present){
    if(!present)return GM_RANGE;
    if(feature>=64)return GM_UNSUPPORTED;
    uint8_t value;int rc=gm_read(memory,GS_BASE+0x274u+feature,&value,1);if(rc)return rc;
    *present=value!=0;return GM_OK;
}
