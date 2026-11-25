#include "guest_character_type.h"
#include <stdlib.h>
struct GuestCharacterTypes {uint16_t values[65536][3];};
static uint32_t word(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t dword(const uint8_t *p){return word(p)|(word(p+2)<<16);}
void gct_destroy(GuestCharacterTypes *types){free(types);}
int gct_create(const uint8_t *raw,uint32_t size,GuestCharacterTypes **out){
    if(!raw || !out)return GM_RANGE;
    if(size<16)return GM_UNSUPPORTED;
    uint32_t begin=dword(raw+8),end=dword(raw+12);
    if(begin<16 || (uint64_t)begin+4>end || end>size)return GM_UNSUPPORTED;
    /* block length: Windows stores it without the 4-byte block header, Wine's
     * generated sortdefault.nls with it; the blocks are otherwise identical */
    if((uint64_t)begin+word(raw+begin)+4!=end && (uint64_t)begin+word(raw+begin)!=end)return GM_UNSUPPORTED;
    uint32_t values=begin+4,index=begin+word(raw+begin+2)+2;
    if(index<values || (index-values)%6 || (uint64_t)index+512>end)return GM_UNSUPPORTED;
    GuestCharacterTypes *types=malloc(sizeof(*types));if(!types)return GM_ALLOCATION;
    for(uint32_t ch=0;ch<65536;ch++){
        uint32_t level1=index+word(raw+index+(ch>>8)*2);
        uint32_t at=level1+((ch>>4)&15)*2;
        if((uint64_t)at+2>end || at<index){free(types);return GM_UNSUPPORTED;}
        uint32_t level2=index+word(raw+at)+(ch&15);
        if(level2>=end || level2<index){free(types);return GM_UNSUPPORTED;}
        uint32_t triple=values+raw[level2]*6;
        if((uint64_t)triple+6>index){free(types);return GM_UNSUPPORTED;}
        for(unsigned kind=0;kind<3;kind++)types->values[ch][kind]=(uint16_t)word(raw+triple+kind*2);
    }
    *out=types;return GM_OK;
}
int gct_string_type(const GuestCharacterTypes *types,GuestMemory *memory,uint32_t kind,uint32_t input,int32_t count,uint32_t output,uint32_t *success,uint32_t *error){
    if(!types || !memory || !success || !error)return GM_UNSUPPORTED;
    if(!input || (kind!=1 && kind!=2 && kind!=4) || input==output){*success=0;*error=87;return GM_OK;}
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
    if(!n){*success=1;*error=0;return GM_OK;}
    if((uint64_t)input+n*2>output && (uint64_t)output+n*2>input)return GM_UNSUPPORTED;
    int rc=gm_check(memory,input,n*2,GM_READ);if(rc)return rc;
    rc=gm_check(memory,output,n*2,GM_WRITE);if(rc)return rc;
    uint8_t *source=malloc(n*2),*result=malloc(n*2);
    if(!source || !result){free(source);free(result);return GM_ALLOCATION;}
    rc=gm_read(memory,input,source,n*2);
    if(!rc){
        for(uint32_t i=0;i<n;i++){
            uint16_t flags=types->values[word(source+i*2)][kind/2];result[i*2]=(uint8_t)flags;result[i*2+1]=(uint8_t)(flags>>8);
        }
        rc=gm_write(memory,output,result,n*2);
    }
    free(source);free(result);if(rc)return rc;
    *success=1;*error=0;return GM_OK;
}
