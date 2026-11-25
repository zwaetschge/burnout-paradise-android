#include "guest_startup_info.h"
#include <string.h>
static uint32_t word(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
int gsi_get(GuestMemory *memory,uint32_t peb,uint32_t output){
    if((uint64_t)peb+0x14>UINT64_C(0x100000000))return GM_RANGE;
    uint8_t pointer[4],parameters[0x90],result[68]={0};
    int rc=gm_read(memory,peb+0x10,pointer,4);if(rc)return rc;
    rc=gm_read(memory,word(pointer),parameters,sizeof(parameters));if(rc)return rc;
    if(word(parameters)<sizeof(parameters) || word(parameters+4)<sizeof(parameters))return GM_UNSUPPORTED;
    result[0]=68;
    memcpy(result+8,parameters+0x7c,4); /* DesktopInfo.Buffer */
    memcpy(result+12,parameters+0x74,4); /* WindowTitle.Buffer */
    memcpy(result+16,parameters+0x4c,32); /* position, dimensions, fill, flags */
    memcpy(result+48,parameters+0x6c,2);
    memcpy(result+50,parameters+0x8a,2);
    if(result[50] || result[51])memcpy(result+52,parameters+0x8c,4);
    uint32_t n=56;
    if(word(parameters+0x68)&0x100){memcpy(result+56,parameters+0x18,12);n=68;}
    /* The supplied Kernelbase leaves standard-handle output fields untouched
     * when STARTF_USESTDHANDLES is absent. Do not zero caller-owned tail bytes. */
    return gm_write(memory,output,result,n);
}

int gsi_std_handle(GuestMemory *memory,uint32_t peb,uint32_t selector,uint32_t *value,uint32_t *error){
    if(!value || !error)return GM_RANGE;
    uint32_t offset;
    if(selector==UINT32_MAX-9)offset=0x18;
    else if(selector==UINT32_MAX-10)offset=0x1c;
    else if(selector==UINT32_MAX-11)offset=0x20;
    else {*value=UINT32_MAX;*error=6;return GM_OK;} /* Frozen Kernelbase: INVALID_HANDLE */
    if((uint64_t)peb+0x14>UINT64_C(0x100000000))return GM_RANGE;
    uint8_t b[4];int rc=gm_read(memory,peb+0x10,b,4);if(rc)return rc;
    uint32_t parameters=word(b);
    if((uint64_t)parameters+offset+4>UINT64_C(0x100000000))return GM_RANGE;
    rc=gm_read(memory,parameters+offset,b,4);if(rc)return rc;
    *value=word(b);*error=0;return GM_OK;
}
