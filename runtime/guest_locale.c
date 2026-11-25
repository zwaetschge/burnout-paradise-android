#include "guest_locale.h"
#include <string.h>
#include <stdlib.h>
int gl_initialize(GuestLocale *locale,uint32_t ansi){
    if(!locale)return GM_RANGE;
    if(ansi!=1252)return GM_UNSUPPORTED;
    memset(locale,0,sizeof(*locale));locale->ansi_code_page=ansi;return GM_OK;
}
int gl_ansi_code_page(const GuestLocale *locale,uint32_t *value){
    if(!locale || !value)return GM_RANGE;
    if(locale->ansi_code_page!=1252)return GM_UNSUPPORTED;
    *value=locale->ansi_code_page;return GM_OK;
}

static uint32_t u16(const uint8_t *b){return (uint32_t)b[0]|((uint32_t)b[1]<<8);}
int gl_install_cp1252(GuestLocale *locale,const uint8_t *nls,uint32_t size){
    /* NLS format reference: Wine locale_private.h, format documentation only.
     * Restrict to the frozen single-byte table; no DBCS/glyph-table fallback. */
    if(!locale || !nls)return GM_RANGE;
    if(locale->ansi_code_page!=1252 || size!=66082 || u16(nls)!=13 || u16(nls+2)!=1252 || u16(nls+4)!=1)return GM_UNSUPPORTED;
    for(unsigned i=14;i<26;i++)if(nls[i])return GM_UNSUPPORTED;
    uint32_t wide=26+u16(nls+26)*2;
    if(wide!=544 || wide+2+65536!=size || u16(nls+540) || u16(nls+542) || u16(nls+wide))return GM_UNSUPPORTED;
    for(unsigned i=0;i<256;i++){
        uint32_t unicode=u16(nls+28+i*2);
        if(nls[wide+2+unicode]!=i)return GM_UNSUPPORTED;
    }
    for(unsigned i=0;i<256;i++)locale->decode[i]=(uint16_t)u16(nls+28+i*2);
    memset(locale->info,0,sizeof(locale->info));locale->info[0]=(uint8_t)u16(nls+4);
    memcpy(locale->info+4,nls+6,2);memcpy(locale->info+6,nls+14,12);
    memcpy(locale->encode,nls+wide+2,sizeof(locale->encode));
    locale->installed=1;return GM_OK;
}
int gl_valid_code_page(const GuestLocale *locale,uint32_t code,uint32_t *value){
    if(!locale || !value)return GM_RANGE;
    if(!locale->installed || code!=1252)return GM_UNSUPPORTED;
    *value=1;return GM_OK;
}
int gl_decode_byte(const GuestLocale *locale,uint32_t byte,uint32_t *value){
    if(!locale || !value || byte>255)return GM_RANGE;
    if(!locale->installed)return GM_UNSUPPORTED;
    *value=locale->decode[byte];return GM_OK;
}

int gl_cp_info(const GuestLocale *locale,GuestMemory *memory,uint32_t code,uint32_t output,uint32_t *success,uint32_t *error){
    if(!locale || !memory || !success || !error)return GM_RANGE;
    if(!output){*success=0;*error=87;return GM_OK;}
    if(code==0)code=locale->ansi_code_page;
    if(!locale->installed || code!=1252)return GM_UNSUPPORTED;
    int rc=gm_write(memory,output,locale->info,sizeof(locale->info));if(rc)return rc;
    *success=1;*error=0;return GM_OK;
}

int gl_mb_to_wide(const GuestLocale *locale,GuestMemory *memory,uint32_t code,uint32_t flags,uint32_t input,int32_t bytes,uint32_t output,int32_t capacity,uint32_t *result,uint32_t *error){
    if(!locale || !memory || !result || !error)return GM_RANGE;
    if(!input || !bytes || bytes < -1 || capacity<0 || (!output && capacity) || input==output){
        *result=0;*error=87;return GM_OK;
    }
    if(code==0)code=locale->ansi_code_page;
    if(!locale->installed || code!=1252 || (flags!=0 && flags!=1 && flags!=8 && flags!=9))return GM_UNSUPPORTED;
    uint32_t count;
    if(bytes==-1){
        int terminated=0;
        for(count=0;count<0x100000u;count++){
            if((uint64_t)input+count>=UINT64_C(0x100000000))return GM_RANGE;
            uint8_t b;int rc=gm_read(memory,input+count,&b,1);if(rc)return rc;
            if(!b){count++;terminated=1;break;}
        }
        if(!terminated)return GM_UNSUPPORTED;
    }else count=(uint32_t)bytes;
    if(count>0x100000u)return GM_UNSUPPORTED; /* bounded startup conversion */
    int rc=gm_check(memory,input,count,GM_READ);if(rc)return rc;
    if(!capacity){*result=count;*error=0;return GM_OK;}
    if((uint32_t)capacity<count){*result=0;*error=122;return GM_OK;}
    if((uint64_t)input+count>output && (uint64_t)output+count*2>input)return GM_UNSUPPORTED;
    rc=gm_check(memory,output,count*2,GM_WRITE);if(rc)return rc;
    uint8_t *source=malloc(count),*converted=malloc(count*2);
    if(!source || !converted){free(source);free(converted);return GM_ALLOCATION;}
    rc=gm_read(memory,input,source,count);
    if(!rc){
        for(uint32_t i=0;i<count;i++){
            uint16_t value=locale->decode[source[i]];
            converted[i*2]=(uint8_t)value;converted[i*2+1]=(uint8_t)(value>>8);
        }
        rc=gm_write(memory,output,converted,count*2);
    }
    free(source);free(converted);if(rc)return rc;
    *result=count;*error=0;return GM_OK;
}

int gl_wide_to_mb(const GuestLocale *locale,GuestMemory *memory,uint32_t code,uint32_t flags,uint32_t input,int32_t count,uint32_t output,int32_t capacity,int default_char,uint32_t *used_default,uint32_t *result,uint32_t *error){
    if(!locale || !memory || !result || !error)return GM_RANGE;
    if(!input || !count || count < -1 || capacity<0 || (!output && capacity) || input==output){*result=0;*error=87;return GM_OK;}
    if(code==0)code=locale->ansi_code_page;
    if(!locale->installed || code!=1252 || flags)return GM_UNSUPPORTED;
    uint32_t n;
    if(count<0){
        int terminated=0;
        for(n=0;n<0x100000;n++){
            if((uint64_t)input+n*2+2>UINT64_C(0x100000000))return GM_RANGE;
            uint8_t bytes[2];int rc=gm_read(memory,input+n*2,bytes,2);if(rc)return rc;
            if(!u16(bytes)){n++;terminated=1;break;}
        }
        if(!terminated)return GM_UNSUPPORTED;
    }else n=(uint32_t)count;
    if(n>0x100000)return GM_UNSUPPORTED;
    int rc=gm_check(memory,input,n*2,GM_READ);if(rc)return rc;
    uint8_t *source=malloc(n*2);if(!source)return GM_ALLOCATION;
    rc=gm_read(memory,input,source,n*2);if(rc){free(source);return rc;}
    for(uint32_t i=0;i<n;i++)if(u16(source+i*2)>=0xd800 && u16(source+i*2)<=0xdfff){free(source);return GM_UNSUPPORTED;}
    if(!capacity){free(source);*result=n;*error=0;return GM_OK;}
    uint32_t produced=(uint32_t)capacity<n ? (uint32_t)capacity : n;
    if((uint64_t)input+n*2>output && (uint64_t)output+produced>input){free(source);return GM_UNSUPPORTED;}
    rc=gm_check(memory,output,produced,GM_WRITE);if(rc){free(source);return rc;}
    uint8_t *converted=malloc(produced);if(!converted){free(source);return GM_ALLOCATION;}
    uint32_t used=0;uint8_t dflt=(default_char>=0)?(uint8_t)default_char:0x3F;
    for(uint32_t i=0;i<produced;i++){
        uint16_t w=u16(source+i*2);uint8_t by=locale->encode[w];
        if(locale->decode[by]!=w){by=dflt;used=1;}   /* not round-trip representable -> default char */
        converted[i]=by;
    }
    if(used_default)*used_default=used;
    rc=gm_write(memory,output,converted,produced);free(source);free(converted);if(rc)return rc;
    *result=produced<n ? 0 : n;*error=produced<n ? 122 : 0;return GM_OK;
}
