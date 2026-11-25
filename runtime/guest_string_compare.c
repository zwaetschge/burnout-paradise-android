#include "guest_string_compare.h"
#include <stdlib.h>
/* One folding table: the same map the Python model uses for lstrcmpiW. */
struct GuestStringCompare { uint16_t fold[65536]; };
enum { GLS_MAX_UNITS = 0x10000 };
static uint32_t word(const uint8_t *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8);}
static uint32_t dword(const uint8_t *p){return word(p)|(word(p+2)<<16);}
void gls_destroy(GuestStringCompare *compare){free(compare);}
/* Three-level index (high byte, high nibble, low nibble) with a 16-bit delta. */
static int character(const uint8_t *raw,uint32_t base,uint32_t end,uint32_t ch,uint16_t *out){
    uint64_t at=(uint64_t)base+(ch>>8)*2;
    if(at+2>end)return GM_UNSUPPORTED;
    at=(uint64_t)base+(word(raw+at)+((ch>>4)&15))*2;
    if(at+2>end)return GM_UNSUPPORTED;
    at=(uint64_t)base+(word(raw+at)+(ch&15))*2;
    if(at+2>end)return GM_UNSUPPORTED;
    *out=(uint16_t)(ch+word(raw+at));return GM_OK;
}
int gls_create(const uint8_t *raw,uint32_t size,GuestStringCompare **out){
    if(!raw || !out)return GM_RANGE;
    if(size<16)return GM_UNSUPPORTED;
    uint32_t begin=dword(raw+4),end=dword(raw+8);
    if(begin<16 || (uint64_t)begin+4>end || end>size || word(raw+begin)!=1)return GM_UNSUPPORTED;
    uint32_t upper=begin+4;uint64_t lower=(uint64_t)upper+word(raw+begin+2)*2;
    if(lower<=upper || lower>=end)return GM_UNSUPPORTED;
    GuestStringCompare *compare=malloc(sizeof(*compare));if(!compare)return GM_ALLOCATION;
    for(uint32_t ch=0;ch<65536;ch++){
        int rc=character(raw,upper,(uint32_t)lower,ch,&compare->fold[ch]);
        if(rc){free(compare);return rc;}
    }
    *out=compare;return GM_OK;
}
/* Bounded UTF-16 read up to and excluding the terminator. */
static int read_operand(GuestMemory *memory,uint32_t pointer,uint16_t *units,uint32_t *length){
    if(!pointer)return GM_RANGE;
    for(uint32_t index=0;index<GLS_MAX_UNITS;index++){
        if((uint64_t)pointer+index*2+2>UINT64_C(0x100000000))return GM_RANGE;
        uint8_t bytes[2];
        int rc=gm_read(memory,pointer+index*2,bytes,2);if(rc)return rc;
        uint32_t ch=word(bytes);
        if(!ch){*length=index;return GM_OK;}
        if(ch>=0xd800 && ch<=0xdfff)return GM_UNSUPPORTED;
        units[index]=(uint16_t)ch;
    }
    return GM_UNSUPPORTED; /* unterminated operand */
}
int gls_compare(const GuestStringCompare *compare,GuestMemory *memory,uint32_t left,uint32_t right,int32_t *result){
    if(!compare || !memory || !result)return GM_UNSUPPORTED;
    uint16_t *first=malloc(sizeof(uint16_t)*GLS_MAX_UNITS);
    uint16_t *second=malloc(sizeof(uint16_t)*GLS_MAX_UNITS);
    if(!first || !second){free(first);free(second);return GM_ALLOCATION;}
    uint32_t first_length=0,second_length=0;
    int rc=read_operand(memory,left,first,&first_length);
    if(!rc)rc=read_operand(memory,right,second,&second_length);
    if(rc){free(first);free(second);return rc;}
    uint32_t shared=first_length<second_length ? first_length : second_length;
    for(uint32_t i=0;i<shared;i++){
        uint16_t a=compare->fold[first[i]],b=compare->fold[second[i]];
        if(a!=b){*result=(int32_t)a-(int32_t)b;free(first);free(second);return GM_OK;}
    }
    *result=(int32_t)first_length-(int32_t)second_length;
    free(first);free(second);return GM_OK;
}
