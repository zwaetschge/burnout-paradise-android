#include "guest_case_map.h"
#include <stdlib.h>
struct GuestCaseMaps {uint16_t values[65536][2];};
static uint32_t word(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t dword(const uint8_t *p){return word(p)|(word(p+2)<<16);}
void gcm_destroy(GuestCaseMaps *maps){free(maps);}
static int character(const uint8_t *raw,uint32_t base,uint32_t end,uint32_t ch,uint16_t *out){
    uint64_t at=(uint64_t)base+(ch>>8)*2;
    if(at+2>end)return GM_UNSUPPORTED;
    at=(uint64_t)base+(word(raw+at)+((ch>>4)&15))*2;
    if(at+2>end)return GM_UNSUPPORTED;
    at=(uint64_t)base+(word(raw+at)+(ch&15))*2;
    if(at+2>end)return GM_UNSUPPORTED;
    *out=(uint16_t)(ch+word(raw+at));return GM_OK;
}
int gcm_create(const uint8_t *raw,uint32_t size,GuestCaseMaps **out){
    if(!raw || !out)return GM_RANGE;
    if(size<16)return GM_UNSUPPORTED;
    uint32_t begin=dword(raw+4),end=dword(raw+8);
    if(begin<16 || (uint64_t)begin+4>end || end>size || word(raw+begin)!=1)return GM_UNSUPPORTED;
    uint32_t upper=begin+4;uint64_t lower=(uint64_t)upper+word(raw+begin+2)*2;
    if(lower<=upper || lower>=end)return GM_UNSUPPORTED;
    GuestCaseMaps *maps=malloc(sizeof(*maps));if(!maps)return GM_ALLOCATION;
    for(uint32_t ch=0;ch<65536;ch++){
        int rc=character(raw,(uint32_t)lower,end,ch,&maps->values[ch][0]);
        if(!rc)rc=character(raw,upper,(uint32_t)lower,ch,&maps->values[ch][1]);
        if(rc){free(maps);return rc;}
    }
    *out=maps;return GM_OK;
}
int gcm_string(const GuestCaseMaps *maps,GuestMemory *memory,uint32_t flags,uint32_t input,int32_t count,uint32_t output,int32_t capacity,uint32_t *result,uint32_t *error){
    if(!maps || !memory || !result || !error)return GM_UNSUPPORTED;
    if(!input || !count || capacity<0 || (!output && capacity)){*result=0;*error=87;return GM_OK;}
    if(flags!=0x100 && flags!=0x200)return GM_UNSUPPORTED;
    uint32_t n;
    if(count<0){
        int terminated=0;
        for(n=0;n<0x100000;n++){
            if((uint64_t)input+n*2+2>UINT64_C(0x100000000))return GM_RANGE;
            uint8_t bytes[2];int rc=gm_read(memory,input+n*2,bytes,2);if(rc)return rc;
            if(!word(bytes)){n++;terminated=1;break;}
        }
        if(!terminated)return GM_UNSUPPORTED;
    }else n=(uint32_t)count;
    if(n>0x100000)return GM_UNSUPPORTED;
    int rc=gm_check(memory,input,n*2,GM_READ);if(rc)return rc;
    uint8_t *source=malloc(n*2);if(!source)return GM_ALLOCATION;
    rc=gm_read(memory,input,source,n*2);if(rc){free(source);return rc;}
    for(uint32_t i=0;i<n;i++)if(word(source+i*2)>=0xd800 && word(source+i*2)<=0xdfff){free(source);return GM_UNSUPPORTED;}
    if(!capacity){free(source);*result=n;*error=0;return GM_OK;}
    if((uint32_t)capacity<n){free(source);*result=0;*error=122;return GM_OK;}
    if(input!=output && (uint64_t)input+n*2>output && (uint64_t)output+n*2>input){free(source);return GM_UNSUPPORTED;}
    rc=gm_check(memory,output,n*2,GM_WRITE);if(rc){free(source);return rc;}
    for(uint32_t i=0;i<n;i++){
        uint16_t mapped=maps->values[word(source+i*2)][flags==0x200];source[i*2]=(uint8_t)mapped;source[i*2+1]=(uint8_t)(mapped>>8);
    }
    rc=gm_write(memory,output,source,n*2);free(source);if(rc)return rc;
    *result=n;*error=0;return GM_OK;
}
