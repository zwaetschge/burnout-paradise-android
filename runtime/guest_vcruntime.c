/* vcruntime140.dll bridges.
 *
 * The recovered EXE imports five memory/string primitives from vcruntime140:
 * memchr, memmove, strchr, strrchr and strstr. They are byte-exact C semantics
 * over guest memory (gm_read/gm_write only, no host pointer reaches the guest).
 *
 * _CxxThrowException is exported for completeness because every C++ exception
 * path in the guest ends there. The port has no SEH/unwind model, so it is a
 * named stop that reports the thrown object and ThrowInfo addresses instead of
 * pretending the exception was handled. */
#include "guest_crt.h"
#include <stdlib.h>
#include <string.h>

#define GCRT_MAX_STRING 0x100000u
#define GCRT_COPY_CHUNK 4096u

/* Never read across a guest 4-KB page boundary: the page after the string may
 * legitimately be unmapped, and a chunked read must not turn that into a
 * model stop. */
static uint32_t page_chunk(uint32_t address,uint32_t limit){
    uint32_t room=4096u-(address&4095u);
    return room<limit ? room : limit;
}

static int crt_memchr(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,value,count;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    uint8_t wanted=(uint8_t)value;
    uint8_t chunk[GCRT_COPY_CHUNK];
    uint32_t position=0;
    while(position<count){
        uint32_t want=count-position;
        if(want>sizeof(chunk))want=sizeof(chunk);
        if(want>page_chunk(address+position,want))want=page_chunk(address+position,want);
        rc=gm_read(gcrt_memory(crt),address+position,chunk,want);
        if(rc)return rc;
        for(uint32_t i=0;i<want;i++)
            if(chunk[i]==wanted){ret->eax=address+position+i;return GM_OK;}
        position+=want;
    }
    ret->eax=0;return GM_OK;
}
static int crt_memmove(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t dest,src,count;
    int rc=gcrt_arg32(crt,sp,0,&dest);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&src);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(rc)return rc;
    if(count){
        uint8_t chunk[GCRT_COPY_CHUNK];
        int backward=(uint64_t)dest>(uint64_t)src && (uint64_t)dest<(uint64_t)src+count;
        if(backward){
            uint32_t remaining=count;
            while(remaining){
                uint32_t want=remaining>sizeof(chunk)?sizeof(chunk):remaining;
                uint32_t offset=remaining-want;
                rc=gm_read(gcrt_memory(crt),src+offset,chunk,want);
                if(rc)return rc;
                rc=gm_write(gcrt_memory(crt),dest+offset,chunk,want);
                if(rc)return rc;
                remaining-=want;
            }
        }else{
            uint32_t done=0;
            while(done<count){
                uint32_t want=count-done;
                if(want>sizeof(chunk))want=sizeof(chunk);
                rc=gm_read(gcrt_memory(crt),src+done,chunk,want);
                if(rc)return rc;
                rc=gm_write(gcrt_memory(crt),dest+done,chunk,want);
                if(rc)return rc;
                done+=want;
            }
        }
    }
    ret->eax=dest;return GM_OK;
}
static int find_byte(GuestCrt *crt,uint32_t address,uint8_t wanted,int include_nul,
                     uint32_t *hit,int *terminated){
    uint8_t chunk[GCRT_COPY_CHUNK];
    uint32_t position=0;
    *hit=0;*terminated=0;
    for(;;){
        uint32_t want=page_chunk(address+position,sizeof(chunk));
        if(position>GCRT_MAX_STRING)return gcrt_stop(crt,"crt string: unterminated string");
        int rc=gm_read(gcrt_memory(crt),address+position,chunk,want);
        if(rc)return rc;
        for(uint32_t i=0;i<want;i++){
            uint8_t byte=chunk[i];
            if(byte==wanted && (include_nul || byte)){
                *hit=address+position+i;return GM_OK;
            }
            if(!byte){*terminated=1;return GM_OK;}
        }
        position+=want;
    }
}
static int crt_strchr(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,value;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&value);
    if(rc)return rc;
    uint32_t hit=0;
    int terminated=0;
    rc=find_byte(crt,address,(uint8_t)value,1,&hit,&terminated);
    if(rc)return rc;
    ret->eax=hit;return GM_OK;
}
static int crt_strrchr(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,value;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&value);
    if(rc)return rc;
    uint8_t wanted=(uint8_t)value;
    uint8_t chunk[GCRT_COPY_CHUNK];
    uint32_t position=0,hit=0;
    for(;;){
        if(position>GCRT_MAX_STRING)return gcrt_stop(crt,"strrchr: unterminated string");
        uint32_t want=page_chunk(address+position,sizeof(chunk));
        int rc=gm_read(gcrt_memory(crt),address+position,chunk,want);
        if(rc)return rc;
        for(uint32_t i=0;i<want;i++){
            uint8_t byte=chunk[i];
            if(byte==wanted)hit=address+position+i;
            if(!byte){ret->eax=hit;return GM_OK;}
        }
        position+=want;
    }
}
static int crt_strstr(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t haystack,needle;
    int rc=gcrt_arg32(crt,sp,0,&haystack);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&needle);
    if(rc)return rc;
    uint32_t needle_length=0;
    rc=gcrt_strlen_ansi(crt,needle,GCRT_MAX_STRING,&needle_length);
    if(rc==GM_RANGE)return gcrt_stop(crt,"strstr: unterminated needle");
    if(rc)return rc;
    if(!needle_length){ret->eax=haystack;return GM_OK;}
    uint8_t *pattern=malloc(needle_length);
    if(!pattern)return GM_ALLOCATION;
    rc=gm_read(gcrt_memory(crt),needle,pattern,needle_length);
    if(rc){free(pattern);return rc;}
    uint8_t *window=malloc(needle_length);
    if(!window){free(pattern);return GM_ALLOCATION;}
    uint32_t position=0;
    for(;;){
        if(position>GCRT_MAX_STRING){
            free(pattern);free(window);
            return gcrt_stop(crt,"strstr: unterminated haystack");
        }
        uint8_t byte;
        rc=gm_read(gcrt_memory(crt),haystack+position,&byte,1);
        if(rc){free(pattern);free(window);return rc;}
        if(!byte)break;
        /* Byte-wise comparison: the needle may cross a page boundary. */
        int match=1;
        for(uint32_t i=0;i<needle_length;i++){
            uint8_t candidate;
            rc=gm_read(gcrt_memory(crt),haystack+position+i,&candidate,1);
            if(rc){free(pattern);free(window);return rc;}
            if(candidate!=pattern[i]){match=0;break;}
            if(!candidate)break;
        }
        if(match){
            free(pattern);free(window);
            ret->eax=haystack+position;return GM_OK;
        }
        position++;
    }
    free(pattern);free(window);
    ret->eax=0;return GM_OK;
}
static int crt_cxx_throw(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t object,throw_info;
    int rc=gcrt_arg32(crt,sp,0,&object);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&throw_info);
    if(rc)return rc;
    (void)object;(void)throw_info;(void)ret;
    return gcrt_stop(crt,"_CxxThrowException: no SEH/unwind model in the port");
}

#define VCRUNTIME_DLL "vcruntime140.dll"
const GcrtExport vcruntime_exports[]={
    {VCRUNTIME_DLL,"memchr",0,crt_memchr},
    {VCRUNTIME_DLL,"memmove",0,crt_memmove},
    {VCRUNTIME_DLL,"strchr",0,crt_strchr},
    {VCRUNTIME_DLL,"strrchr",0,crt_strrchr},
    {VCRUNTIME_DLL,"strstr",0,crt_strstr},
    {VCRUNTIME_DLL,"_CxxThrowException",0,crt_cxx_throw}
};
const uint32_t vcruntime_export_count=sizeof(vcruntime_exports)/sizeof(vcruntime_exports[0]);
