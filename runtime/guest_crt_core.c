/* Shared state and helpers for the 32-bit guest CRT bridges.
 *
 * This file owns the GuestCrt context (guest memory, process heap, TEB, the
 * borrowed locale and file API, the guest-callback and clock hooks), the
 * argument/string/heap/encoding helpers every CRT module uses, and the lookup
 * over all CRT registration tables. See guest_crt.h for the calling
 * convention and the documented model limits. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "guest_crt.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct GuestCrt {
    GuestMemory *memory;
    GuestProcessHeap *heap;
    uint32_t heap_handle,teb;
    const GuestLocale *locale;
    GuestFileApi *files;
    GcrtGuestCall guest_call;
    void *guest_call_user;
    GcrtUnixSeconds clock;
    void *clock_user;
    int32_t bias_minutes;
    uint32_t errno_address;
    uint32_t std_files[3];
    uint32_t tmpfile_counter;
    uint32_t rand_state;
    uint32_t scratch[GCRT_SLOT_COUNT];
    char stop[192];
    char error_text[192];
};

int gcrt_create(GuestMemory *memory,GuestProcessHeap *heap,uint32_t heap_handle,
                uint32_t teb,const GuestLocale *locale,GuestFileApi *files,
                GuestCrt **out){
    if(!memory || !out)return GM_RANGE;
    GuestCrt *crt=calloc(1,sizeof(*crt));
    if(!crt)return GM_ALLOCATION;
    crt->memory=memory;crt->heap=heap;crt->heap_handle=heap_handle;
    crt->teb=teb;crt->locale=locale;crt->files=files;
    crt->rand_state=1;                    /* UCRT: srand(1) is the default */
    crt->stop[0]=0;crt->error_text[0]=0;
    *out=crt;return GM_OK;
}
void gcrt_destroy(GuestCrt *crt){free(crt);}
GuestMemory *gcrt_memory(GuestCrt *crt){return crt ? crt->memory : NULL;}
uint32_t gcrt_teb(const GuestCrt *crt){return crt ? crt->teb : 0;}
uint32_t gcrt_heap_handle(const GuestCrt *crt){return crt ? crt->heap_handle : 0;}
const GuestLocale *gcrt_locale(const GuestCrt *crt){return crt ? crt->locale : NULL;}
GuestFileApi *gcrt_files(GuestCrt *crt){return crt ? crt->files : NULL;}

int gcrt_set_guest_call(GuestCrt *crt,GcrtGuestCall fn,void *user){
    if(!crt)return GM_RANGE;
    crt->guest_call=fn;crt->guest_call_user=user;return GM_OK;
}
int gcrt_set_clock(GuestCrt *crt,GcrtUnixSeconds fn,void *user){
    if(!crt)return GM_RANGE;
    crt->clock=fn;crt->clock_user=user;return GM_OK;
}
int gcrt_set_timezone_bias(GuestCrt *crt,int32_t bias_minutes){
    if(!crt)return GM_RANGE;
    crt->bias_minutes=bias_minutes;return GM_OK;
}
int32_t gcrt_timezone_bias(const GuestCrt *crt){return crt ? crt->bias_minutes : 0;}
int gcrt_set_errno_address(GuestCrt *crt,uint32_t address){
    if(!crt)return GM_RANGE;
    crt->errno_address=address;return GM_OK;
}

int gcrt_stop(GuestCrt *crt,const char *reason){
    if(crt){
        size_t n=reason ? strlen(reason) : 0;
        if(n>=sizeof(crt->stop))n=sizeof(crt->stop)-1;
        if(n)memcpy(crt->stop,reason,n);
        crt->stop[n]=0;
    }
    return GM_UNSUPPORTED;
}
const char *gcrt_stop_reason(const GuestCrt *crt){
    return crt && crt->stop[0] ? crt->stop : "";
}

/* ------------------------------------------------------------------ */
/* Argument access                                                      */
/* ------------------------------------------------------------------ */

int gcrt_arg32(GuestCrt *crt,uint32_t sp,uint32_t index,uint32_t *value){
    if(!crt || !value)return GM_RANGE;
    uint64_t address=(uint64_t)sp+4+(uint64_t)index*4;
    if(address+4>UINT64_C(0x100000000))return GM_RANGE;
    uint8_t bytes[4];
    int rc=gm_read(crt->memory,(uint32_t)address,bytes,4);
    if(rc)return rc;
    *value=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|
           ((uint32_t)bytes[3]<<24);
    return GM_OK;
}
int gcrt_arg64(GuestCrt *crt,uint32_t sp,uint32_t index,uint64_t *value){
    if(!crt || !value)return GM_RANGE;
    uint32_t low,high;
    int rc=gcrt_arg32(crt,sp,index,&low);
    if(!rc)rc=gcrt_arg32(crt,sp,index+1,&high);
    if(rc)return rc;
    *value=((uint64_t)high<<32)|low;return GM_OK;
}

/* ------------------------------------------------------------------ */
/* errno                                                                */
/* ------------------------------------------------------------------ */

int gcrt_errno_address(GuestCrt *crt,uint32_t *address){
    if(!crt || !address)return GM_RANGE;
    if(crt->errno_address){*address=crt->errno_address;return GM_OK;}
    if(!crt->scratch[GCRT_SLOT_ERRNO]){
        uint32_t block=0;
        int rc=gcrt_heap_alloc(crt,8,4,&block);
        if(rc)return rc;
        if(!block)return gcrt_stop(crt,"crt errno: guest heap exhausted");
        crt->scratch[GCRT_SLOT_ERRNO]=block;
    }
    crt->errno_address=crt->scratch[GCRT_SLOT_ERRNO];
    *address=crt->errno_address;return GM_OK;
}
int gcrt_errno(const GuestCrt *crt,uint32_t *value){
    if(!crt || !value)return GM_RANGE;
    uint32_t address=crt->errno_address;
    if(!address)return GM_UNSUPPORTED;
    uint8_t bytes[4];
    int rc=gm_read(crt->memory,address,bytes,4);
    if(rc)return rc;
    *value=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|
           ((uint32_t)bytes[3]<<24);
    return GM_OK;
}
int gcrt_set_errno(GuestCrt *crt,uint32_t value){
    uint32_t address;
    int rc=gcrt_errno_address(crt,&address);
    if(rc)return rc;
    uint8_t bytes[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),
                      (uint8_t)(value>>24)};
    return gm_write(crt->memory,address,bytes,4);
}

/* Lazily allocated per-context guest scratch block (tm, strerror, _ecvt). */
int gcrt_scratch(GuestCrt *crt,uint32_t slot,uint32_t size,uint32_t *address){
    if(!crt || !address || slot>=GCRT_SLOT_COUNT)return GM_RANGE;
    if(!crt->scratch[slot]){
        uint32_t block=0;
        int rc=gcrt_heap_alloc(crt,8,size,&block);
        if(rc)return rc;
        if(!block)return gcrt_stop(crt,"crt scratch: guest heap exhausted");
        crt->scratch[slot]=block;
    }
    *address=crt->scratch[slot];return GM_OK;
}
uint32_t *gcrt_std_file_slot(GuestCrt *crt,uint32_t index){
    if(!crt || index>2)return NULL;
    return &crt->std_files[index];
}
uint32_t *gcrt_tmpfile_counter(GuestCrt *crt){
    return crt ? &crt->tmpfile_counter : NULL;
}
uint32_t *gcrt_wcstok_cursor(GuestCrt *crt){
    return crt ? &crt->scratch[GCRT_SLOT_WCSTOK] : NULL;
}
uint32_t *gcrt_rand_state(GuestCrt *crt){
    return crt ? &crt->rand_state : NULL;
}
GcrtGuestCall gcrt_guest_call(GuestCrt *crt,void **user){
    if(!crt)return NULL;
    if(user)*user=crt->guest_call_user;
    return crt->guest_call;
}
int64_t gcrt_host_seconds(void){
    struct timespec now;
    if(clock_gettime(CLOCK_REALTIME,&now))return 0;
    return (int64_t)now.tv_sec;
}
int64_t gcrt_now_seconds(GuestCrt *crt){
    if(crt && crt->clock)return crt->clock(crt->clock_user);
    return gcrt_host_seconds();
}

/* ------------------------------------------------------------------ */
/* Guest strings                                                        */
/* ------------------------------------------------------------------ */

int gcrt_read_ansi(GuestCrt *crt,uint32_t address,char *out,uint32_t capacity,
                   uint32_t *length){
    if(!crt || !out || !capacity)return GM_RANGE;
    for(uint32_t i=0;i<capacity;i++){
        uint8_t byte;
        int rc=gm_read(crt->memory,address+i,&byte,1);
        if(rc)return rc;
        out[i]=(char)byte;
        if(!byte){if(length)*length=i;return GM_OK;}
    }
    return GM_RANGE;   /* no terminator inside the caller's bound */
}
int gcrt_read_wide(GuestCrt *crt,uint32_t address,uint16_t *out,uint32_t capacity,
                   uint32_t *length){
    if(!crt || !out || !capacity)return GM_RANGE;
    for(uint32_t i=0;i<capacity;i++){
        uint8_t bytes[2];
        int rc=gm_read(crt->memory,address+i*2,bytes,2);
        if(rc)return rc;
        uint16_t unit=(uint16_t)(bytes[0]|(bytes[1]<<8));
        out[i]=unit;
        if(!unit){if(length)*length=i;return GM_OK;}
    }
    return GM_RANGE;
}
int gcrt_write_ansi(GuestCrt *crt,uint32_t address,const char *text,uint32_t capacity){
    if(!crt || !text)return GM_RANGE;
    size_t n=strlen(text)+1;
    if(n>capacity)return GM_RANGE;
    return gm_write(crt->memory,address,text,(uint32_t)n);
}
int gcrt_write_wide(GuestCrt *crt,uint32_t address,const uint16_t *text,
                    uint32_t capacity){
    if(!crt || !text)return GM_RANGE;
    uint32_t n=0;
    while(text[n])n++;
    if(n+1>capacity)return GM_RANGE;
    uint8_t *bytes=malloc((n+1)*2);
    if(!bytes)return GM_ALLOCATION;
    for(uint32_t i=0;i<=n;i++){
        bytes[i*2]=(uint8_t)text[i];bytes[i*2+1]=(uint8_t)(text[i]>>8);
    }
    int rc=gm_write(crt->memory,address,bytes,(n+1)*2);
    free(bytes);return rc;
}
int gcrt_strlen_ansi(GuestCrt *crt,uint32_t address,uint32_t limit,uint32_t *length){
    if(!crt || !length)return GM_RANGE;
    for(uint32_t i=0;i<limit;i++){
        uint8_t byte;
        int rc=gm_read(crt->memory,address+i,&byte,1);
        if(rc)return rc;
        if(!byte){*length=i;return GM_OK;}
    }
    return GM_RANGE;
}
int gcrt_strlen_wide(GuestCrt *crt,uint32_t address,uint32_t limit,uint32_t *length){
    if(!crt || !length)return GM_RANGE;
    for(uint32_t i=0;i<limit;i++){
        uint8_t bytes[2];
        int rc=gm_read(crt->memory,address+i*2,bytes,2);
        if(rc)return rc;
        if(!bytes[0] && !bytes[1]){*length=i;return GM_OK;}
    }
    return GM_RANGE;
}

/* ------------------------------------------------------------------ */
/* Heap                                                                 */
/* ------------------------------------------------------------------ */

int gcrt_heap_alloc(GuestCrt *crt,uint32_t flags,uint32_t size,uint32_t *address){
    if(!crt || !address)return GM_RANGE;
    *address=0;
    if(!crt->heap)return gcrt_stop(crt,"crt heap: no process heap attached");
    return gph_alloc(crt->heap,crt->heap_handle,flags,size,address);
}
int gcrt_heap_free(GuestCrt *crt,uint32_t address){
    if(!crt)return GM_RANGE;
    if(!address)return GM_OK;                       /* free(NULL) is a no-op */
    if(!crt->heap)return gcrt_stop(crt,"crt heap: no process heap attached");
    return gph_free(crt->heap,crt->heap_handle,0,address);
}
int gcrt_heap_size(GuestCrt *crt,uint32_t address,uint32_t *size){
    if(!crt || !size)return GM_RANGE;
    if(!crt->heap)return gcrt_stop(crt,"crt heap: no process heap attached");
    return gph_size(crt->heap,crt->heap_handle,0,address,size);
}
int gcrt_heap_realloc(GuestCrt *crt,uint32_t flags,uint32_t address,uint32_t size,
                      uint32_t *result){
    if(!crt || !result)return GM_RANGE;
    *result=0;
    if(!crt->heap)return gcrt_stop(crt,"crt heap: no process heap attached");
    if(!address)return gph_alloc(crt->heap,crt->heap_handle,flags,size,result);
    return gph_realloc(crt->heap,crt->heap_handle,flags,address,size,result);
}

/* ------------------------------------------------------------------ */
/* CP1252 <-> UTF-16                                                    */
/* ------------------------------------------------------------------ */

int gcrt_ansi_to_wide(GuestCrt *crt,const char *text,uint32_t units,uint16_t *out,
                      uint32_t capacity,uint32_t *written){
    if(!crt || !text || !out || !written)return GM_RANGE;
    if(!crt->locale || !crt->locale->installed)
        return gcrt_stop(crt,"crt convert: no installed code page");
    if(units>capacity)return GM_RANGE;
    for(uint32_t i=0;i<units;i++){
        uint32_t cp=0;
        int rc=gl_decode_byte(crt->locale,(uint8_t)text[i],&cp);
        if(rc)return rc;
        if(cp>0xffff)return gcrt_stop(crt,"crt convert: code point above BMP");
        out[i]=(uint16_t)cp;
    }
    *written=units;return GM_OK;
}
int gcrt_wide_to_ansi(GuestCrt *crt,const uint16_t *text,uint32_t units,char *out,
                      uint32_t capacity,uint32_t *written){
    if(!crt || !text || !out || !written)return GM_RANGE;
    if(!crt->locale || !crt->locale->installed)
        return gcrt_stop(crt,"crt convert: no installed code page");
    if(units>capacity)return GM_RANGE;
    for(uint32_t i=0;i<units;i++){
        if(text[i]>=0xd800 && text[i]<=0xdfff)
            return gcrt_stop(crt,"crt convert: UTF-16 surrogate");
        /* Unmappable code points become 0x00, exactly like the existing
         * WideCharToMultiByte bridge with the default default-char. */
        out[i]=(char)crt->locale->encode[text[i]];
    }
    *written=units;return GM_OK;
}

/* ------------------------------------------------------------------ */
/* Registration lookup                                                  */
/* ------------------------------------------------------------------ */

static int same_dll(const char *a,const char *b){
    while(*a && *b){
        char x=*a++,y=*b++;
        if(x>='A'&&x<='Z')x=(char)(x-'A'+'a');
        if(y>='A'&&y<='Z')y=(char)(y-'A'+'a');
        if(x!=y)return 0;
    }
    return *a==0 && *b==0;
}
static const GcrtExport *const tables[]={
    crt_string_exports,crt_stdio_exports,crt_convert_exports,crt_heap_exports,
    crt_misc_exports,vcruntime_exports,msvcp_exports
};
static const uint32_t *const counts[]={
    &crt_string_export_count,&crt_stdio_export_count,&crt_convert_export_count,
    &crt_heap_export_count,&crt_misc_export_count,&vcruntime_export_count,
    &msvcp_export_count
};
const GcrtExport *gcrt_lookup(const char *dll,const char *name){
    if(!dll || !name)return NULL;
    for(size_t t=0;t<sizeof(tables)/sizeof(tables[0]);t++){
        const GcrtExport *table=tables[t];
        for(uint32_t i=0;i<*counts[t];i++)
            if(same_dll(table[i].dll,dll) && !strcmp(table[i].name,name))return &table[i];
    }
    return NULL;
}
uint32_t gcrt_export_total(void){
    uint32_t total=0;
    for(size_t t=0;t<sizeof(counts)/sizeof(counts[0]);t++)total+=*counts[t];
    return total;
}
