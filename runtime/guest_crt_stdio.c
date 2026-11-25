/* UCRT stdio bridges (api-ms-win-crt-stdio-l1-1-0.dll).
 *
 * Two parts:
 *
 *  1. A guest FILE object. A FILE* is the guest address of a 32-byte guest
 *     block owned by the process heap:
 *         +0x00 magic 'FILE'   +0x04 guest file handle (guest_file_api)
 *         +0x08 flags          +0x0c pushback byte or 0xffffffff
 *     The guest never sees a host FILE, fd or pointer. Reads/writes go through
 *     runtime/guest_file_api.c, so the guest path projection (A7) and the
 *     shared handle table (runtime/guest_handles.h) are used unchanged.
 *     stdin/stdout/stderr are guest FILE objects with the console flag: a GUI
 *     process without an inherited console reads EOF on stdin and discards
 *     writes (nothing is printed to the host).
 *
 *  2. The printf/scanf engines. Formatting is implemented here - the host
 *     vsnprintf is never given a guest pointer or a guest argument. Only the
 *     decimal conversion of one already-decoded host double uses the host
 *     snprintf into a private buffer, then MSVC's differences are applied
 *     (three-digit %e exponents, "1.#INF"/"1.#QNAN", 13-digit %a).
 *     Supported: %d %i %u %o %x %X %c %C %s %S %p %n %f %F %e %E %g %G %a %A
 *     %%, flags - + space # 0, width and precision (including *), and the
 *     length modifiers h hh l ll L I32 I64 I z j t w. Varargs are walked with
 *     the MSVC x86 va_list rule (4-byte slots, 8 bytes for double/__int64, no
 *     extra alignment) because that is how the guest's own va_list cursor
 *     advances.
 *
 * Documented limits: no stream buffering (setvbuf accepted and ignored), no
 * FILE locking, no _IOBUF/fpos_t API, no _popen/_pclose (no process creation),
 * tmpfile() creates a real file in the guest user profile and does not delete
 * it on close (delete-on-close is not modelled), and scanf supports the
 * conversions listed above but not %a hex-float input beyond strtod's own
 * hex parsing. */
#include "guest_crt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Guest FILE objects                                                   */
/* ------------------------------------------------------------------ */

#define GCRT_FILE_SIZE 32u
#define GCRT_FILE_MAGIC 0x454c4946u   /* 'F','I','L','E' */
enum { GCRT_FILE_READABLE=1, GCRT_FILE_WRITABLE=2, GCRT_FILE_CONSOLE=4,
       GCRT_FILE_EOF=8, GCRT_FILE_ERROR=16, GCRT_FILE_APPEND=32 };

typedef struct { uint32_t handle,flags,pushback; } FileObj;

static int file_load(GuestCrt *crt,uint32_t address,FileObj *out){
    if(!crt || !out || !address)return GM_RANGE;
    uint8_t bytes[GCRT_FILE_SIZE];
    int rc=gm_read(gcrt_memory(crt),address,bytes,GCRT_FILE_SIZE);
    if(rc)return rc;
    uint32_t magic=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|
                   ((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
    if(magic!=GCRT_FILE_MAGIC)return gcrt_stop(crt,"stdio: FILE* is not a guest FILE object");
    out->handle=(uint32_t)bytes[4]|((uint32_t)bytes[5]<<8)|
                ((uint32_t)bytes[6]<<16)|((uint32_t)bytes[7]<<24);
    out->flags=(uint32_t)bytes[8]|((uint32_t)bytes[9]<<8)|
               ((uint32_t)bytes[10]<<16)|((uint32_t)bytes[11]<<24);
    out->pushback=(uint32_t)bytes[12]|((uint32_t)bytes[13]<<8)|
                  ((uint32_t)bytes[14]<<16)|((uint32_t)bytes[15]<<24);
    return GM_OK;
}
static int file_store(GuestCrt *crt,uint32_t address,const FileObj *file){
    uint8_t bytes[GCRT_FILE_SIZE];
    memset(bytes,0,sizeof(bytes));
    bytes[0]=(uint8_t)GCRT_FILE_MAGIC;bytes[1]=(uint8_t)(GCRT_FILE_MAGIC>>8);
    bytes[2]=(uint8_t)(GCRT_FILE_MAGIC>>16);bytes[3]=(uint8_t)(GCRT_FILE_MAGIC>>24);
    bytes[4]=(uint8_t)file->handle;bytes[5]=(uint8_t)(file->handle>>8);
    bytes[6]=(uint8_t)(file->handle>>16);bytes[7]=(uint8_t)(file->handle>>24);
    bytes[8]=(uint8_t)file->flags;bytes[9]=(uint8_t)(file->flags>>8);
    bytes[10]=(uint8_t)(file->flags>>16);bytes[11]=(uint8_t)(file->flags>>24);
    bytes[12]=(uint8_t)file->pushback;bytes[13]=(uint8_t)(file->pushback>>8);
    bytes[14]=(uint8_t)(file->pushback>>16);bytes[15]=(uint8_t)(file->pushback>>24);
    return gm_write(gcrt_memory(crt),address,bytes,GCRT_FILE_SIZE);
}
static int file_create(GuestCrt *crt,uint32_t handle,uint32_t flags,uint32_t *address){
    uint32_t block=0;
    int rc=gcrt_heap_alloc(crt,8,GCRT_FILE_SIZE,&block);
    if(rc)return rc;
    if(!block)return gcrt_stop(crt,"stdio: guest heap exhausted for FILE object");
    FileObj file={handle,flags,UINT32_MAX};
    rc=file_store(crt,block,&file);
    if(rc)return rc;
    *address=block;return GM_OK;
}
static int guest_last_error(GuestCrt *crt,uint32_t *value){
    uint8_t bytes[4];
    int rc=gm_read(gcrt_memory(crt),gcrt_teb(crt)+0x34,bytes,4);
    if(rc)return rc;
    *value=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|
           ((uint32_t)bytes[3]<<24);
    return GM_OK;
}

/* ---- raw transfers (host buffers, guest file handles) -------------- */

static int file_read_host(GuestCrt *crt,FileObj *file,void *out,uint32_t count,
                          uint32_t *done){
    if(!crt || !file || !done)return GM_RANGE;
    *done=0;
    if(!count)return GM_OK;
    if(file->pushback!=UINT32_MAX){
        ((uint8_t *)out)[0]=(uint8_t)file->pushback;
        file->pushback=UINT32_MAX;
        *done=1;
        return GM_OK;
    }
    if(file->flags&GCRT_FILE_CONSOLE)return GM_OK;      /* GUI stdin: EOF */
    uint32_t buffer=0,slot=0;
    int rc=gcrt_scratch(crt,GCRT_SLOT_IOBUF,1024,&buffer);
    if(!rc)rc=gcrt_scratch(crt,GCRT_SLOT_IO,4,&slot);
    if(rc)return rc;
    uint32_t total=0;
    while(total<count){
        uint32_t want=count-total;
        if(want>1024)want=1024;
        uint32_t result=0;
        rc=gfa_read_file(gcrt_memory(crt),gcrt_teb(crt),file->handle,buffer,want,slot,
                         0,&result);
        if(rc)return rc;
        if(!result){                       /* BOOL success flag, not a count */
            file->flags|=GCRT_FILE_ERROR;
            break;
        }
        uint8_t count_bytes[4];
        rc=gm_read(gcrt_memory(crt),slot,count_bytes,4);
        if(rc)return rc;
        uint32_t got=(uint32_t)count_bytes[0]|((uint32_t)count_bytes[1]<<8)|
                     ((uint32_t)count_bytes[2]<<16)|((uint32_t)count_bytes[3]<<24);
        if(got>want)got=want;
        if(got){
            rc=gm_read(gcrt_memory(crt),buffer,(uint8_t *)out+total,got);
            if(rc)return rc;
        }
        if(!got){                          /* successful read of zero bytes: EOF */
            file->flags|=GCRT_FILE_EOF;
            break;
        }
        total+=got;
        if(got<want)break;
    }
    *done=total;return GM_OK;
}
static int file_write_host(GuestCrt *crt,FileObj *file,const void *in,uint32_t count,
                           uint32_t *done){
    if(!crt || !file || !done)return GM_RANGE;
    *done=0;
    if(!count)return GM_OK;
    if(file->flags&GCRT_FILE_CONSOLE){*done=count;return GM_OK;}  /* discarded */
    uint32_t buffer=0,slot=0;
    int rc=gcrt_scratch(crt,GCRT_SLOT_IOBUF,1024,&buffer);
    if(!rc)rc=gcrt_scratch(crt,GCRT_SLOT_IO,4,&slot);
    if(rc)return rc;
    uint32_t total=0;
    while(total<count){
        uint32_t want=count-total;
        if(want>1024)want=1024;
        rc=gm_write(gcrt_memory(crt),buffer,(const uint8_t *)in+total,want);
        if(rc)return rc;
        uint32_t result=0;
        rc=gfa_write_file(gcrt_memory(crt),gcrt_teb(crt),file->handle,buffer,want,slot,
                          0,&result);
        if(rc)return rc;
        if(!result){file->flags|=GCRT_FILE_ERROR;break;}
        uint8_t count_bytes[4];
        rc=gm_read(gcrt_memory(crt),slot,count_bytes,4);
        if(rc)return rc;
        uint32_t put=(uint32_t)count_bytes[0]|((uint32_t)count_bytes[1]<<8)|
                     ((uint32_t)count_bytes[2]<<16)|((uint32_t)count_bytes[3]<<24);
        if(put>want)put=want;
        total+=put;
        if(put<want)break;
    }
    *done=total;return GM_OK;
}
static int file_seek(GuestCrt *crt,FileObj *file,int64_t offset,uint32_t method,
                     uint32_t *position){
    if(!crt || !file || !position)return GM_RANGE;
    if(file->flags&GCRT_FILE_CONSOLE)return gcrt_stop(crt,"stdio: seek on a console stream");
    uint32_t result=0;
    int rc=gfa_set_file_pointer(gcrt_memory(crt),gcrt_teb(crt),file->handle,
                                (uint32_t)((uint64_t)offset&0xffffffffu),
                                (uint32_t)((uint64_t)offset>>32),method,&result);
    if(rc)return rc;
    if(result==0xffffffffu){
        uint32_t error=0;
        if(!guest_last_error(crt,&error))file->flags|=GCRT_FILE_ERROR;
        *position=0;return GM_OK;
    }
    file->flags&=~GCRT_FILE_EOF;
    *position=result;return GM_OK;
}
static int file_flush(GuestCrt *crt,FileObj *file,uint32_t *result){
    if(!crt || !file || !result)return GM_RANGE;
    if(file->flags&GCRT_FILE_CONSOLE){*result=1;return GM_OK;}
    return gfa_flush_file_buffers(gcrt_memory(crt),gcrt_teb(crt),file->handle,result);
}
static int file_close(GuestCrt *crt,FileObj *file,uint32_t *result){
    if(!crt || !file || !result)return GM_RANGE;
    if(file->flags&GCRT_FILE_CONSOLE){*result=1;return GM_OK;}
    return gfa_close_handle(gcrt_memory(crt),gcrt_teb(crt),file->handle,result);
}

/* ---- std streams --------------------------------------------------- */

static int std_file(GuestCrt *crt,uint32_t index,uint32_t *address){
    if(!crt || !address || index>2)return GM_RANGE;
    uint32_t *slot=gcrt_std_file_slot(crt,index);
    if(!slot)return GM_RANGE;
    if(!*slot){
        uint32_t flags=GCRT_FILE_CONSOLE;
        if(index==0)flags|=GCRT_FILE_READABLE;
        else flags|=GCRT_FILE_WRITABLE;
        int rc=file_create(crt,0,flags,slot);
        if(rc)return rc;
    }
    *address=*slot;return GM_OK;
}

/* ------------------------------------------------------------------ */
/* printf engine                                                        */
/* ------------------------------------------------------------------ */

typedef int (*GcrtWriteFn)(void *user,const void *data,uint32_t bytes);

typedef struct {
    GuestCrt *crt;
    int wide;                 /* output encoding: UTF-16LE units */
    GcrtWriteFn write;
    void *user;
    uint32_t count;           /* characters emitted */
} Fmt;

static int fmt_cp(Fmt *f,uint32_t cp){
    if(f->wide){
        uint8_t bytes[4];
        uint32_t size;
        if(cp<=0xffff){
            bytes[0]=(uint8_t)cp;bytes[1]=(uint8_t)(cp>>8);size=2;
        }else if(cp<=0x10ffff){
            uint32_t v=cp-0x10000;
            uint16_t hi=(uint16_t)(0xd800+(v>>10)),lo=(uint16_t)(0xdc00+(v&0x3ff));
            bytes[0]=(uint8_t)hi;bytes[1]=(uint8_t)(hi>>8);
            bytes[2]=(uint8_t)lo;bytes[3]=(uint8_t)(lo>>8);size=4;
        }else return gcrt_stop(f->crt,"printf: code point out of range");
        int rc=f->write(f->user,bytes,size);
        if(rc)return rc;
        f->count++;return GM_OK;
    }
    uint8_t byte;
    if(cp<0x80)byte=(uint8_t)cp;
    else if(cp<=0xffff){
        const GuestLocale *locale=gcrt_locale(f->crt);
        if(!locale || !locale->installed)
            return gcrt_stop(f->crt,"printf: no code page for wide output");
        byte=locale->encode[cp];
        if(!byte)byte='?';
    }else byte='?';
    int rc=f->write(f->user,&byte,1);
    if(rc)return rc;
    f->count++;return GM_OK;
}
static int fmt_literal(Fmt *f,int from_wide,uint32_t cp){
    if(from_wide)return fmt_cp(f,cp);
    if(!f->wide){
        uint8_t byte=(uint8_t)cp;
        int rc=f->write(f->user,&byte,1);
        if(rc)return rc;
        f->count++;return GM_OK;
    }
    const GuestLocale *locale=gcrt_locale(f->crt);
    if(!locale || !locale->installed)
        return gcrt_stop(f->crt,"printf: no code page for wide output");
    return fmt_cp(f,locale->decode[cp&0xff]);
}
static int fmt_pad(Fmt *f,char pad,uint32_t count){
    for(uint32_t i=0;i<count;i++){
        int rc=fmt_cp(f,(uint8_t)pad);
        if(rc)return rc;
    }
    return GM_OK;
}
static int fmt_text(Fmt *f,const char *text,uint32_t length){
    for(uint32_t i=0;i<length;i++){
        int rc=fmt_cp(f,(uint8_t)text[i]);
        if(rc)return rc;
    }
    return GM_OK;
}

typedef struct { GuestCrt *crt; uint32_t address; } GcrtVa;
static int va_u32(GcrtVa *va,uint32_t *value){
    uint8_t bytes[4];
    int rc=gm_read(gcrt_memory(va->crt),va->address,bytes,4);
    if(rc)return rc;
    *value=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|
           ((uint32_t)bytes[3]<<24);
    va->address+=4;return GM_OK;
}
static int va_u64(GcrtVa *va,uint64_t *value){
    uint8_t bytes[8];
    int rc=gm_read(gcrt_memory(va->crt),va->address,bytes,8);
    if(rc)return rc;
    uint64_t low=(uint64_t)bytes[0]|((uint64_t)bytes[1]<<8)|((uint64_t)bytes[2]<<16)|
                 ((uint64_t)bytes[3]<<24);
    uint64_t high=(uint64_t)bytes[4]|((uint64_t)bytes[5]<<8)|((uint64_t)bytes[6]<<16)|
                  ((uint64_t)bytes[7]<<24);
    *value=(high<<32)|low;
    va->address+=8;return GM_OK;
}
static int va_double(GcrtVa *va,double *value){
    uint64_t bits;
    int rc=va_u64(va,&bits);
    if(rc)return rc;
    memcpy(value,&bits,8);return GM_OK;
}

enum { LEN_NONE=0, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_BIGL, LEN_I32, LEN_I64,
       LEN_I, LEN_Z, LEN_J, LEN_T, LEN_W };

typedef struct {
    int left,plus,space,hash,zero;
    int width,width_star;
    int precision,precision_star,precision_set;
    int length;
    uint32_t type;
} Spec;

typedef struct { GuestCrt *crt; uint32_t address; int wide; } FmtSrc;
static int src_get(FmtSrc *s,uint32_t *cp){
    if(s->wide){
        uint8_t bytes[2];
        int rc=gm_read(gcrt_memory(s->crt),s->address,bytes,2);
        if(rc)return rc;
        s->address+=2;
        *cp=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8);
    }else{
        uint8_t byte;
        int rc=gm_read(gcrt_memory(s->crt),s->address,&byte,1);
        if(rc)return rc;
        s->address+=1;*cp=byte;
    }
    return GM_OK;
}

static int emit_number(Fmt *f,const Spec *sp,uint64_t magnitude,int negative,
                       int base,int upper,const char *prefix,uint32_t prefix_len){
    char digits[72];
    uint32_t ndigits=0;
    const char *alphabet=upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if(magnitude || !sp->precision_set || sp->precision){
        if(!magnitude)digits[ndigits++]='0';
        else while(magnitude){digits[ndigits++]=alphabet[magnitude%(uint32_t)base];
                             magnitude/=(uint32_t)base;}
    }
    uint32_t precision=(uint32_t)(sp->precision_set ? sp->precision : 0);
    if(precision<ndigits)precision=ndigits;
    char sign=negative ? '-' : (sp->plus ? '+' : (sp->space ? ' ' : 0));
    uint32_t total=(sign?1u:0u)+prefix_len+precision;
    int rc=GM_OK;
    if(!sp->left && !(sp->zero && !sp->precision_set) && sp->width>0 &&
       (uint32_t)sp->width>total){
        rc=fmt_pad(f,' ',(uint32_t)sp->width-total);
        if(rc)return rc;
    }
    if(sign){
        rc=fmt_cp(f,(uint8_t)sign);
        if(rc)return rc;
    }
    for(uint32_t i=0;i<prefix_len;i++){
        rc=fmt_cp(f,(uint8_t)prefix[i]);
        if(rc)return rc;
    }
    if(!sp->left && sp->zero && !sp->precision_set && sp->width>0 &&
       (uint32_t)sp->width>total){
        rc=fmt_pad(f,'0',(uint32_t)sp->width-total);
        if(rc)return rc;
    }
    for(uint32_t i=precision;i>ndigits;i--){
        rc=fmt_cp(f,'0');
        if(rc)return rc;
    }
    while(ndigits){
        rc=fmt_cp(f,(uint8_t)digits[--ndigits]);
        if(rc)return rc;
    }
    if(sp->left && sp->width>0 && (uint32_t)sp->width>total){
        rc=fmt_pad(f,' ',(uint32_t)sp->width-total);
        if(rc)return rc;
    }
    return GM_OK;
}

/* MSVC prints at least three exponent digits: 1.0e+000, not 1.0e+00. */
static void fix_exponent(char *buf,size_t size,char marker){
    char *at=strchr(buf,marker);
    if(!at)return;
    char *digits=at+1;
    if(*digits=='+'||*digits=='-')digits++;
    size_t n=0;
    while(digits[n]>='0'&&digits[n]<='9')n++;
    while(n<3){
        size_t length=strlen(digits);
        if(digits+length+2>buf+size)return;
        memmove(digits+1,digits,length+1);
        *digits='0';
        n++;
    }
}

/* MSVC %a: 0x1.<13 hex digits>p<exp>, uppercase for %A. */
static int hex_float_body(char *out,size_t size,double value,int upper,int precision){
    (void)size;
    uint64_t bits;
    memcpy(&bits,&value,8);
    uint32_t exponent=(uint32_t)((bits>>52)&0x7ff);
    uint64_t fraction=bits&UINT64_C(0xfffffffffffff);
    int leading;
    int exp;
    if(value==0.0){leading=0;exp=0;}
    else if(exponent==0){leading=0;exp=-1022;}
    else{leading=1;exp=(int)exponent-1023;}
    if(precision<0)precision=13;
    if(precision>13)precision=13;
    char *p=out;
    const char *marker=upper ? "0X" : "0x";
    for(const char *m=marker;*m;m++)*p++=*m;
    *p++=(char)('0'+leading);
    *p++='.';
    for(int i=0;i<precision;i++){
        uint32_t digit=0;
        if(i<13)digit=(uint32_t)((fraction>>(52-4*(i+1)))&0xf);
        *p++=(char)(digit<10 ? '0'+digit : (upper?'A':'a')+digit-10);
    }
    *p++=upper?'P':'p';
    *p++=exp<0?'-':'+';
    char number[16];
    int n=0;
    unsigned magnitude=(unsigned)(exp<0?-exp:exp);
    if(!magnitude)number[n++]='0';
    while(magnitude){number[n++]=(char)('0'+magnitude%10);magnitude/=10;}
    while(n)*p++=number[--n];
    *p=0;
    return (int)(p-out);
}

static int float_body(Fmt *f,const Spec *sp,double value,uint32_t type){
    int upper=(type=='F'||type=='E'||type=='G'||type=='A');
    int negative=signbit(value)!=0;
    double magnitude=negative ? -value : value;
    char buf[1200];
    int length=0;
    if(isnan(magnitude)){
        const char *text=negative ? "-1.#IND" : "1.#QNAN";
        length=(int)strlen(text);memcpy(buf,text,(size_t)length+1);
        int rc=fmt_pad(f,' ',(uint32_t)(sp->width>length && !sp->left ? sp->width-length : 0));
        if(rc)return rc;
        rc=fmt_text(f,buf,(uint32_t)length);
        if(rc)return rc;
        if(sp->left && sp->width>length)return fmt_pad(f,' ',(uint32_t)(sp->width-length));
        return GM_OK;
    }
    if(isinf(magnitude)){
        const char *text=negative ? "-1.#INF" : "1.#INF";
        length=(int)strlen(text);memcpy(buf,text,(size_t)length+1);
        int rc=fmt_pad(f,' ',(uint32_t)(sp->width>length && !sp->left ? sp->width-length : 0));
        if(rc)return rc;
        rc=fmt_text(f,buf,(uint32_t)length);
        if(rc)return rc;
        if(sp->left && sp->width>length)return fmt_pad(f,' ',(uint32_t)(sp->width-length));
        return GM_OK;
    }
    int precision=sp->precision_set ? sp->precision : 6;
    if(precision<0)precision=0;
    if(precision>512)precision=512;
    if(type=='A'||type=='a'){
        length=hex_float_body(buf,sizeof(buf),magnitude,upper,sp->precision_set?precision:-1);
    }else{
        char form[16];
        char conversion=(char)(type=='F'?'f':type);
        if((conversion=='g'||conversion=='G')&&precision==0)precision=1;
        int n=0;
        form[n++]='%';
        if(sp->hash)form[n++]='#';
        form[n++]='.';
        form[n++]='*';
        form[n++]=conversion;
        form[n]=0;
        int written=snprintf(buf,sizeof(buf),form,precision,magnitude);
        if(written<0)return gcrt_stop(f->crt,"printf: host decimal conversion failed");
        if((size_t)written>=sizeof(buf))written=(int)sizeof(buf)-1;
        length=written;
        if(conversion=='e'||conversion=='g'||conversion=='E'||conversion=='G'){
            fix_exponent(buf,sizeof(buf),(conversion=='E'||conversion=='G')?'E':'e');
            length=(int)strlen(buf);
        }
    }
    /* Sign, then zero padding, then the body, then spaces. */
    uint32_t sign_len=negative||sp->plus||sp->space ? 1u : 0u;
    uint32_t total=sign_len+(uint32_t)length;
    int rc=GM_OK;
    if(!sp->left && !sp->zero && sp->width>0 && (uint32_t)sp->width>total){
        rc=fmt_pad(f,' ',(uint32_t)sp->width-total);
        if(rc)return rc;
    }
    if(sign_len){
        char sign=negative ? '-' : (sp->plus ? '+' : ' ');
        rc=fmt_cp(f,(uint8_t)sign);
        if(rc)return rc;
    }
    if(!sp->left && sp->zero && sp->width>0 && (uint32_t)sp->width>total){
        rc=fmt_pad(f,'0',(uint32_t)sp->width-total);
        if(rc)return rc;
    }
    rc=fmt_text(f,buf,(uint32_t)length);
    if(rc)return rc;
    if(sp->left && sp->width>0 && (uint32_t)sp->width>total)
        return fmt_pad(f,' ',(uint32_t)sp->width-total);
    return GM_OK;
}

/* Reads a guest narrow/wide string character by character and emits it. */
static int emit_string(Fmt *f,uint32_t address,int wide_source,uint32_t limit){
    if(!address){
        /* The UCRT prints "(null)" for a NULL %s/%ls argument. */
        static const char text[]="(null)";
        uint32_t count=6;
        if(limit<count)count=limit;
        return fmt_text(f,text,count);
    }
    for(uint32_t i=0;limit==UINT32_MAX || i<limit;i++){
        uint32_t cp;
        if(wide_source){
            uint8_t bytes[2];
            int rc=gm_read(gcrt_memory(f->crt),address+i*2,bytes,2);
            if(rc)return rc;
            cp=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8);
            if(!cp)return GM_OK;
            if(f->wide){
                rc=fmt_cp(f,cp);
                if(rc)return rc;
            }else{
                const GuestLocale *locale=gcrt_locale(f->crt);
                if(!locale || !locale->installed)
                    return gcrt_stop(f->crt,"printf: no code page for wide output");
                uint8_t byte=locale->encode[cp];
                if(!byte)byte='?';
                rc=f->write(f->user,&byte,1);
                if(rc)return rc;
                f->count++;
            }
        }else{
            uint8_t byte;
            int rc=gm_read(gcrt_memory(f->crt),address+i,&byte,1);
            if(rc)return rc;
            if(!byte)return GM_OK;
            if(f->wide){
                const GuestLocale *locale=gcrt_locale(f->crt);
                if(!locale || !locale->installed)
                    return gcrt_stop(f->crt,"printf: no code page for wide output");
                rc=fmt_cp(f,locale->decode[byte]);
                if(rc)return rc;
            }else{
                rc=f->write(f->user,&byte,1);
                if(rc)return rc;
                f->count++;
            }
        }
    }
    return GM_OK;
}

static int store_count(GuestCrt *crt,uint32_t address,int length,uint32_t count){
    uint8_t bytes[8];
    uint32_t size;
    switch(length){
        case LEN_HH: size=1;break;
        case LEN_H: size=2;break;
        case LEN_LL: case LEN_I64: size=8;break;
        default: size=4;break;
    }
    if(!address)return gcrt_stop(crt,"printf: NULL %n pointer");
    for(uint32_t i=0;i<size;i++)bytes[i]=(uint8_t)(count>>(8*i));
    return gm_write(gcrt_memory(crt),address,bytes,size);
}

static int format_run(GuestCrt *crt,Fmt *f,int wide_format,uint32_t format,GcrtVa *va){
    FmtSrc src={crt,format,wide_format};
    for(;;){
        uint32_t cp;
        int rc=src_get(&src,&cp);
        if(rc)return rc;
        if(!cp)break;
        if(cp!='%'){
            rc=fmt_literal(f,wide_format,cp);
            if(rc)return rc;
            continue;
        }
        Spec sp;
        memset(&sp,0,sizeof(sp));
        rc=src_get(&src,&cp);
        if(rc)return rc;
        for(;;){
            if(cp=='-')sp.left=1;
            else if(cp=='+')sp.plus=1;
            else if(cp==' ')sp.space=1;
            else if(cp=='#')sp.hash=1;
            else if(cp=='0')sp.zero=1;
            else break;
            rc=src_get(&src,&cp);
            if(rc)return rc;
        }
        if(cp=='*'){
            uint32_t value;
            rc=va_u32(va,&value);
            if(rc)return rc;
            sp.width=(int)value;sp.width_star=1;
            if(sp.width<0){sp.left=1;sp.width=-sp.width;}
            rc=src_get(&src,&cp);
            if(rc)return rc;
        }else while(cp>='0'&&cp<='9'){
            sp.width=sp.width*10+(int)(cp-'0');
            rc=src_get(&src,&cp);
            if(rc)return rc;
        }
        if(cp=='.'){
            sp.precision_set=1;
            rc=src_get(&src,&cp);
            if(rc)return rc;
            if(cp=='*'){
                uint32_t value;
                rc=va_u32(va,&value);
                if(rc)return rc;
                sp.precision=(int)value;sp.precision_star=1;
                if(sp.precision<0)sp.precision_set=0;
                rc=src_get(&src,&cp);
                if(rc)return rc;
            }else{
                while(cp>='0'&&cp<='9'){
                    sp.precision=sp.precision*10+(int)(cp-'0');
                    rc=src_get(&src,&cp);
                    if(rc)return rc;
                }
            }
        }
        for(;;){
            if(cp=='h'){
                rc=src_get(&src,&cp);
                if(rc)return rc;
                if(cp=='h'){sp.length=LEN_HH;rc=src_get(&src,&cp);if(rc)return rc;}
                else sp.length=LEN_H;
            }else if(cp=='l'){
                rc=src_get(&src,&cp);
                if(rc)return rc;
                if(cp=='l'){sp.length=LEN_LL;rc=src_get(&src,&cp);if(rc)return rc;}
                else sp.length=LEN_L;
            }else if(cp=='L'){sp.length=LEN_BIGL;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='I'){
                rc=src_get(&src,&cp);
                if(rc)return rc;
                if(cp=='6'){
                    rc=src_get(&src,&cp);
                    if(rc)return rc;
                    if(cp!='4')return gcrt_stop(crt,"printf: bad I64 length");
                    sp.length=LEN_I64;rc=src_get(&src,&cp);if(rc)return rc;
                }else if(cp=='3'){
                    rc=src_get(&src,&cp);
                    if(rc)return rc;
                    if(cp!='2')return gcrt_stop(crt,"printf: bad I32 length");
                    sp.length=LEN_I32;rc=src_get(&src,&cp);if(rc)return rc;
                }else sp.length=LEN_I;
            }else if(cp=='z'){sp.length=LEN_Z;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='j'){sp.length=LEN_J;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='t'){sp.length=LEN_T;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='w'){sp.length=LEN_W;rc=src_get(&src,&cp);if(rc)return rc;}
            else break;
        }
        sp.type=cp;
        int wide_string;
        switch(cp){
            case '%':
                rc=fmt_cp(f,'%');
                if(rc)return rc;
                break;
            case 'd': case 'i': {
                uint64_t raw=0;
                if(sp.length==LEN_LL||sp.length==LEN_I64||sp.length==LEN_J){
                    rc=va_u64(va,&raw);
                }else{
                    uint32_t value=0;
                    rc=va_u32(va,&value);
                    if(!rc){
                        if(sp.length==LEN_HH)raw=(uint64_t)(int64_t)(int8_t)value;
                        else if(sp.length==LEN_H)raw=(uint64_t)(int64_t)(int16_t)value;
                        else raw=(uint64_t)(int64_t)(int32_t)value;
                    }
                }
                if(rc)return rc;
                int negative=(int64_t)raw<0;
                uint64_t magnitude=negative ? (uint64_t)(-(int64_t)raw) : raw;
                rc=emit_number(f,&sp,magnitude,negative,10,0,"",0);
                if(rc)return rc;
                break;
            }
            case 'u': case 'o': case 'x': case 'X': {
                uint64_t raw=0;
                if(sp.length==LEN_LL||sp.length==LEN_I64||sp.length==LEN_J){
                    rc=va_u64(va,&raw);
                }else{
                    uint32_t value=0;
                    rc=va_u32(va,&value);
                    if(!rc){
                        if(sp.length==LEN_HH)raw=(uint8_t)value;
                        else if(sp.length==LEN_H)raw=(uint16_t)value;
                        else raw=value;
                    }
                }
                if(rc)return rc;
                int base=cp=='u'?10:(cp=='o'?8:16);
                char prefix[3];uint32_t prefix_len=0;
                if(sp.hash && raw){
                    if(cp=='x'){prefix[0]='0';prefix[1]='x';prefix_len=2;}
                    else if(cp=='X'){prefix[0]='0';prefix[1]='X';prefix_len=2;}
                    else if(cp=='o'){prefix[0]='0';prefix_len=1;}
                }
                rc=emit_number(f,&sp,raw,0,base,cp=='X',prefix,prefix_len);
                if(rc)return rc;
                break;
            }
            case 'c': case 'C': {
                uint32_t value;
                rc=va_u32(va,&value);
                if(rc)return rc;
                int wide_char=(cp=='C') || sp.length==LEN_L || sp.length==LEN_W;
                if(sp.width>1 && !sp.left){
                    rc=fmt_pad(f,' ',(uint32_t)sp.width-1);
                    if(rc)return rc;
                }
                if(wide_char){
                    if(f->wide)rc=fmt_cp(f,value&0xffff);
                    else{
                        const GuestLocale *locale=gcrt_locale(crt);
                        if(!locale || !locale->installed)
                            return gcrt_stop(crt,"printf: no code page for wide output");
                        uint8_t byte=locale->encode[value&0xffff];
                        if(!byte)byte='?';
                        rc=f->write(f->user,&byte,1);
                        if(!rc)f->count++;
                    }
                }else{
                    if(f->wide){
                        const GuestLocale *locale=gcrt_locale(crt);
                        if(!locale || !locale->installed)
                            return gcrt_stop(crt,"printf: no code page for wide output");
                        rc=fmt_cp(f,locale->decode[value&0xff]);
                    }else{
                        uint8_t byte=(uint8_t)value;
                        rc=f->write(f->user,&byte,1);
                        if(!rc)f->count++;
                    }
                }
                if(rc)return rc;
                if(sp.width>1 && sp.left){
                    rc=fmt_pad(f,' ',(uint32_t)sp.width-1);
                    if(rc)return rc;
                }
                break;
            }
            case 's': case 'S': {
                uint32_t address;
                rc=va_u32(va,&address);
                if(rc)return rc;
                if(sp.length==LEN_L||sp.length==LEN_W)wide_string=1;
                else if(sp.length==LEN_H)wide_string=0;
                else wide_string=cp=='S' ? !wide_format : wide_format;
                uint32_t limit=sp.precision_set ? (uint32_t)sp.precision : UINT32_MAX;
                uint32_t before=f->count;
                /* Width padding needs the length first, so measure the string
                 * with a counting-only pass when a width is requested. */
                if(sp.width>0){
                    Fmt probe={crt,f->wide,NULL,NULL,0};
                    int saved=0;
                    (void)saved;
                    uint32_t measured=0;
                    for(uint32_t i=0;limit==UINT32_MAX||i<limit;i++){
                        uint32_t unit;
                        if(wide_string){
                            uint8_t bytes[2];
                            rc=gm_read(gcrt_memory(crt),address+i*2,bytes,2);
                            if(rc)return rc;
                            unit=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8);
                            if(!unit)break;
                        }else{
                            uint8_t byte;
                            rc=gm_read(gcrt_memory(crt),address+i,&byte,1);
                            if(rc)return rc;
                            if(!byte)break;
                            unit=byte;
                        }
                        measured++;
                    }
                    (void)probe;
                    if(!sp.left && (uint32_t)sp.width>measured){
                        rc=fmt_pad(f,' ',(uint32_t)sp.width-measured);
                        if(rc)return rc;
                    }
                }
                rc=emit_string(f,address,wide_string,limit);
                if(rc)return rc;
                uint32_t emitted=f->count-before;
                if(sp.left && sp.width>0 && (uint32_t)sp.width>emitted){
                    rc=fmt_pad(f,' ',(uint32_t)sp.width-emitted);
                    if(rc)return rc;
                }
                break;
            }
            case 'p': {
                uint32_t value;
                rc=va_u32(va,&value);
                if(rc)return rc;
                Spec pointer=sp;
                pointer.hash=0;pointer.plus=0;pointer.space=0;
                if(!pointer.precision_set){pointer.precision_set=1;pointer.precision=8;}
                rc=emit_number(f,&pointer,value,0,16,1,"",0);
                if(rc)return rc;
                break;
            }
            case 'n': {
                uint32_t address;
                rc=va_u32(va,&address);
                if(rc)return rc;
                rc=store_count(crt,address,sp.length,f->count);
                if(rc)return rc;
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            case 'a': case 'A': {
                double value;
                if(sp.length==LEN_BIGL)return gcrt_stop(crt,"printf: long double argument");
                rc=va_double(va,&value);
                if(rc)return rc;
                rc=float_body(f,&sp,value,cp);
                if(rc)return rc;
                break;
            }
            default:
                return gcrt_stop(crt,"printf: unsupported conversion specifier");
        }
    }
    return GM_OK;
}

/* ---- output sinks -------------------------------------------------- */

typedef struct {
    GuestCrt *crt;
    uint32_t address,capacity,written;
    int wide,overflow;
} BufSink;
static int buf_sink_write(void *user,const void *data,uint32_t bytes){
    BufSink *sink=user;
    uint32_t units=sink->wide ? bytes/2 : bytes;
    uint32_t room=0;
    if(sink->address && sink->capacity>sink->written)room=sink->capacity-sink->written;
    uint32_t store=units<room?units:room;
    if(store){
        uint32_t unit_size=sink->wide?2u:1u;
        int rc=gm_write(gcrt_memory(sink->crt),sink->address+sink->written*unit_size,
                        data,store*unit_size);
        if(rc)return rc;
    }
    if(store<units)sink->overflow=1;
    sink->written+=store;
    return GM_OK;
}
typedef struct { GuestCrt *crt; uint32_t file; int error; } FileSink;
static int file_sink_write(void *user,const void *data,uint32_t bytes){
    FileSink *sink=user;
    FileObj file;
    int rc=file_load(sink->crt,sink->file,&file);
    if(rc)return rc;
    uint32_t done=0;
    rc=file_write_host(sink->crt,&file,data,bytes,&done);
    if(rc)return rc;
    rc=file_store(sink->crt,sink->file,&file);
    if(rc)return rc;
    if(done!=bytes){sink->error=1;return GM_OK;}
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* scanf engine                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    GuestCrt *crt;
    int wide_format;
    int from_file;
    uint32_t file;
    uint32_t buffer,position;
    int wide_input;
    int pushed,pushed_valid;
    uint32_t pushed_cp;
    uint32_t pushback[16];
    uint32_t pushback_count;
    uint32_t consumed,assigned;
    int input_failure;
} Scan;

static int scan_get(Scan *s,uint32_t *cp){
    if(s->pushback_count){
        *cp=s->pushback[--s->pushback_count];
        s->consumed++;return GM_OK;
    }
    if(s->from_file){
        FileObj file;
        int rc=file_load(s->crt,s->file,&file);
        if(rc)return rc;
        uint8_t byte;
        uint32_t done=0;
        rc=file_read_host(s->crt,&file,&byte,1,&done);
        if(rc)return rc;
        rc=file_store(s->crt,s->file,&file);
        if(rc)return rc;
        if(!done){*cp=UINT32_MAX;return GM_OK;}
        s->consumed++;*cp=byte;return GM_OK;
    }
    if(s->wide_input){
        uint8_t bytes[2];
        int rc=gm_read(gcrt_memory(s->crt),s->buffer+s->position,bytes,2);
        if(rc)return rc;
        uint16_t unit=(uint16_t)(bytes[0]|(bytes[1]<<8));
        if(!unit){*cp=UINT32_MAX;return GM_OK;}
        s->position+=2;s->consumed++;*cp=unit;return GM_OK;
    }
    uint8_t byte;
    int rc=gm_read(gcrt_memory(s->crt),s->buffer+s->position,&byte,1);
    if(rc)return rc;
    if(!byte){*cp=UINT32_MAX;return GM_OK;}
    s->position+=1;s->consumed++;*cp=byte;return GM_OK;
}
static void scan_unget(Scan *s,uint32_t cp){
    if(cp==UINT32_MAX)return;
    if(s->pushback_count>=sizeof(s->pushback)/sizeof(s->pushback[0]))return;
    s->pushback[s->pushback_count++]=cp;
    if(s->consumed)s->consumed--;
}
static int scan_space(uint32_t cp){
    return cp==' '||(cp>=0x09&&cp<=0x0d);
}
static int scan_skip_space(Scan *s){
    for(;;){
        uint32_t cp;
        int rc=scan_get(s,&cp);
        if(rc)return rc;
        if(cp==UINT32_MAX)return GM_OK;
        if(!scan_space(cp)){scan_unget(s,cp);return GM_OK;}
    }
}

typedef struct {
    int suppress,width,width_set;
    int length;
    uint32_t type;
    uint16_t set[256];
    uint32_t set_count;
    int set_negate;
} ScanSpec;

static int digit_value(uint32_t cp){
    if(cp>='0'&&cp<='9')return (int)(cp-'0');
    if(cp>='a'&&cp<='f')return (int)(cp-'a'+10);
    if(cp>='A'&&cp<='F')return (int)(cp-'A'+10);
    return -1;
}

static int scan_store(GuestCrt *crt,uint32_t address,int length,uint64_t value){
    if(!address)return gcrt_stop(crt,"scanf: NULL output pointer");
    uint32_t size;
    switch(length){
        case LEN_HH: size=1;break;
        case LEN_H: size=2;break;
        case LEN_LL: case LEN_I64: case LEN_J: size=8;break;
        default: size=4;break;
    }
    uint8_t bytes[8];
    for(uint32_t i=0;i<size;i++)bytes[i]=(uint8_t)(value>>(8*i));
    return gm_write(gcrt_memory(crt),address,bytes,size);
}

static int scan_integer(Scan *s,ScanSpec *sp,GcrtVa *va,int base_hint){
    int rc=scan_skip_space(s);
    if(rc)return rc;
    uint32_t cp;
    rc=scan_get(s,&cp);
    if(rc)return rc;
    if(cp==UINT32_MAX){s->input_failure=1;return GM_OK;}
    int negative=0,seen_sign=0;
    if(cp=='+'||cp=='-'){
        negative=cp=='-';seen_sign=1;
        rc=scan_get(s,&cp);
        if(rc)return rc;
    }
    int base=base_hint;
    if(base==0){
        base=10;
        if(cp=='0'){
            rc=scan_get(s,&cp);
            if(rc)return rc;
            if(cp=='x'||cp=='X'){
                base=16;
                rc=scan_get(s,&cp);
                if(rc)return rc;
            }else base=8;
        }
    }else if(base==16 && cp=='0'){
        rc=scan_get(s,&cp);
        if(rc)return rc;
        if(cp=='x'||cp=='X'){
            rc=scan_get(s,&cp);
            if(rc)return rc;
        }
    }
    uint64_t magnitude=0;
    uint32_t digits=0;
    int width=sp->width_set ? sp->width : 0;
    while(cp!=UINT32_MAX && (!width || digits<(uint32_t)width)){
        int value=digit_value(cp);
        if(value<0||value>=base)break;
        magnitude=magnitude*(uint32_t)base+(uint32_t)value;
        digits++;
        rc=scan_get(s,&cp);
        if(rc)return rc;
    }
    scan_unget(s,cp);
    if(!digits){
        if(seen_sign)scan_unget(s,negative?'-':'+');
        return GM_OK;
    }
    if(sp->suppress)return GM_OK;
    int64_t signed_value=negative ? -(int64_t)magnitude : (int64_t)magnitude;
    uint32_t address;
    rc=va_u32(va,&address);
    if(rc)return rc;
    rc=scan_store(s->crt,address,sp->length,(uint64_t)signed_value);
    if(rc)return rc;
    s->assigned++;
    return GM_OK;
}
static int scan_float(Scan *s,ScanSpec *sp,GcrtVa *va){
    int rc=scan_skip_space(s);
    if(rc)return rc;
    char token[512];
    uint32_t length=0;
    int width=sp->width_set ? sp->width : 0;
    uint32_t cp;
    for(;;){
        rc=scan_get(s,&cp);
        if(rc)return rc;
        if(cp==UINT32_MAX)break;
        int accept=0;
        if((cp>='0'&&cp<='9')||cp=='.'||cp=='+'||cp=='-'||cp=='e'||cp=='E'||
           cp=='x'||cp=='X'||cp=='p'||cp=='P'||cp=='a'||cp=='A'||cp=='b'||cp=='B'||
           cp=='c'||cp=='C'||cp=='d'||cp=='D'||cp=='f'||cp=='F'||cp=='i'||cp=='I'||
           cp=='n'||cp=='N'||cp=='s'||cp=='S'||cp=='t'||cp=='T'||cp=='y'||cp=='Y')
            accept=1;
        if(!accept || (width && length>=(uint32_t)width)){scan_unget(s,cp);break;}
        if(length+1>=sizeof(token))return gcrt_stop(s->crt,"scanf: float token too long");
        token[length++]=(char)cp;
    }
    token[length]=0;
    if(!length)return GM_OK;
    char *end=NULL;
    double value=strtod(token,&end);
    if(end==token)return GM_OK;
    /* Push back the unconsumed tail of the token. */
    for(char *p=token+length;p>end;){
        p--;
        scan_unget(s,(uint32_t)(uint8_t)*p);
    }
    if(sp->suppress)return GM_OK;
    uint32_t address;
    rc=va_u32(va,&address);
    if(rc)return rc;
    if(!address)return gcrt_stop(s->crt,"scanf: NULL output pointer");
    if(sp->length==LEN_L||sp->length==LEN_BIGL){
        uint8_t bytes[8];
        uint64_t bits;memcpy(&bits,&value,8);
        for(uint32_t i=0;i<8;i++)bytes[i]=(uint8_t)(bits>>(8*i));
        rc=gm_write(gcrt_memory(s->crt),address,bytes,8);
    }else{
        float single=(float)value;
        uint32_t bits;memcpy(&bits,&single,4);
        uint8_t bytes[4];
        for(uint32_t i=0;i<4;i++)bytes[i]=(uint8_t)(bits>>(8*i));
        rc=gm_write(gcrt_memory(s->crt),address,bytes,4);
    }
    if(rc)return rc;
    s->assigned++;
    return GM_OK;
}
static int scan_chars(Scan *s,ScanSpec *sp,GcrtVa *va,int string_mode,int set_mode){
    uint32_t width=sp->width_set ? (uint32_t)sp->width
                                 : ((string_mode||set_mode)?UINT32_MAX:1u);
    if(!string_mode && !set_mode){
        /* %c does not skip leading whitespace. */
    }else{
        int rc=scan_skip_space(s);
        if(rc)return rc;
    }
    uint32_t address=0;
    if(!sp->suppress){
        int rc=va_u32(va,&address);
        if(rc)return rc;
        if(!address)return gcrt_stop(s->crt,"scanf: NULL output pointer");
    }
    uint32_t count=0;
    for(;;){
        if(width!=UINT32_MAX && count>=width)break;
        uint32_t cp;
        int rc=scan_get(s,&cp);
        if(rc)return rc;
        if(cp==UINT32_MAX){
            if(!count)s->input_failure=1;
            break;
        }
        if(set_mode){
            int member=0;
            for(uint32_t i=0;i<sp->set_count;i++)if(sp->set[i]==cp){member=1;break;}
            if(sp->set_negate)member=!member;
            if(!member){scan_unget(s,cp);break;}
        }else if(string_mode){
            if(scan_space(cp)){scan_unget(s,cp);break;}
        }
        if(!sp->suppress){
            uint8_t byte=(uint8_t)cp;
            rc=gm_write(gcrt_memory(s->crt),address+count,&byte,1);
            if(rc)return rc;
        }
        count++;
    }
    if(!count)return GM_OK;
    if(!sp->suppress){
        if(string_mode||set_mode){
            uint8_t zero=0;
            int rc=gm_write(gcrt_memory(s->crt),address+count,&zero,1);
            if(rc)return rc;
        }
        s->assigned++;
    }
    return GM_OK;
}
static int parse_set(FmtSrc *src,ScanSpec *sp){
    /* The opening '[' has been consumed; read up to the closing ']' from the
     * format string (not from the input). */
    uint32_t cp;
    int rc=src_get(src,&cp);
    if(rc)return rc;
    if(cp=='^'){sp->set_negate=1;rc=src_get(src,&cp);if(rc)return rc;}
    int first=1;
    sp->set_count=0;
    while(cp!=UINT32_MAX && cp!=0 && (cp!=']'||first)){
        first=0;
        uint32_t low=cp;
        rc=src_get(src,&cp);
        if(rc)return rc;
        if(cp=='-'){
            uint32_t next;
            rc=src_get(src,&next);
            if(rc)return rc;
            if(next!=UINT32_MAX && next!=0 && next!=']'){
                for(uint32_t v=low;v<=next && sp->set_count<256;v++)sp->set[sp->set_count++]=(uint16_t)v;
                rc=src_get(src,&cp);
                if(rc)return rc;
                continue;
            }
            if(sp->set_count<256)sp->set[sp->set_count++]=(uint16_t)low;
            if(sp->set_count<256)sp->set[sp->set_count++]='-';
            cp=next;
            continue;
        }
        if(sp->set_count<256)sp->set[sp->set_count++]=(uint16_t)low;
    }
    return GM_OK;
}
static int scan_run(GuestCrt *crt,Scan *s,uint32_t format,GcrtVa *va){
    FmtSrc src={crt,format,s->wide_format};
    for(;;){
        uint32_t cp;
        int rc=src_get(&src,&cp);
        if(rc)return rc;
        if(!cp)break;
        if(scan_space(cp)){
            rc=scan_skip_space(s);
            if(rc)return rc;
            continue;
        }
        if(cp!='%'){
            uint32_t input;
            rc=scan_get(s,&input);
            if(rc)return rc;
            if(input!=cp){scan_unget(s,input);break;}
            continue;
        }
        rc=src_get(&src,&cp);
        if(rc)return rc;
        if(cp=='%'){
            uint32_t input;
            rc=scan_get(s,&input);
            if(rc)return rc;
            if(input!='%'){scan_unget(s,input);break;}
            continue;
        }
        ScanSpec sp;
        memset(&sp,0,sizeof(sp));
        if(cp=='*'){sp.suppress=1;rc=src_get(&src,&cp);if(rc)return rc;}
        while(cp>='0'&&cp<='9'){
            sp.width=sp.width*10+(int)(cp-'0');sp.width_set=1;
            rc=src_get(&src,&cp);
            if(rc)return rc;
        }
        for(;;){
            if(cp=='h'){
                rc=src_get(&src,&cp);
                if(rc)return rc;
                if(cp=='h'){sp.length=LEN_HH;rc=src_get(&src,&cp);if(rc)return rc;}
                else sp.length=LEN_H;
            }else if(cp=='l'){
                rc=src_get(&src,&cp);
                if(rc)return rc;
                if(cp=='l'){sp.length=LEN_LL;rc=src_get(&src,&cp);if(rc)return rc;}
                else sp.length=LEN_L;
            }else if(cp=='L'){sp.length=LEN_BIGL;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='I'){
                rc=src_get(&src,&cp);
                if(rc)return rc;
                if(cp=='6'){
                    rc=src_get(&src,&cp);
                    if(rc)return rc;
                    if(cp!='4')return gcrt_stop(crt,"scanf: bad I64 length");
                    sp.length=LEN_I64;rc=src_get(&src,&cp);if(rc)return rc;
                }else if(cp=='3'){
                    rc=src_get(&src,&cp);
                    if(rc)return rc;
                    if(cp!='2')return gcrt_stop(crt,"scanf: bad I32 length");
                    sp.length=LEN_I32;rc=src_get(&src,&cp);if(rc)return rc;
                }else sp.length=LEN_I;
            }else if(cp=='z'){sp.length=LEN_Z;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='j'){sp.length=LEN_J;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='t'){sp.length=LEN_T;rc=src_get(&src,&cp);if(rc)return rc;}
            else if(cp=='w'){sp.length=LEN_W;rc=src_get(&src,&cp);if(rc)return rc;}
            else break;
        }
        sp.type=cp;
        switch(cp){
            case 'd': rc=scan_integer(s,&sp,va,10);break;
            case 'i': rc=scan_integer(s,&sp,va,0);break;
            case 'u': rc=scan_integer(s,&sp,va,10);break;
            case 'o': rc=scan_integer(s,&sp,va,8);break;
            case 'x': case 'X': case 'p': rc=scan_integer(s,&sp,va,16);break;
            case 'c': case 'C': rc=scan_chars(s,&sp,va,0,0);break;
            case 's': case 'S': rc=scan_chars(s,&sp,va,1,0);break;
            case '[':
                rc=parse_set(&src,&sp);
                if(!rc)rc=scan_chars(s,&sp,va,0,1);
                break;
            case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
            case 'a': case 'A': rc=scan_float(s,&sp,va);break;
            case 'n': {
                uint32_t address;
                rc=va_u32(va,&address);
                if(rc)return rc;
                rc=scan_store(crt,address,sp.length,s->consumed);
                if(rc)return rc;
                break;
            }
            default:
                return gcrt_stop(crt,"scanf: unsupported conversion specifier");
        }
        if(rc)return rc;
        if(s->input_failure)break;
    }
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* Bridge entry points                                                  */
/* ------------------------------------------------------------------ */

static int result_eax(GcrtReturn *ret,uint32_t value){ret->eax=value;return GM_OK;}

static int crt_acrt_iob_func(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t index,address=0;
    int rc=gcrt_arg32(crt,sp,0,&index);
    if(rc)return rc;
    if(index>2)return result_eax(ret,0);
    rc=std_file(crt,index,&address);
    if(rc)return rc;
    ret->eax=address;return GM_OK;
}

/* Shared implementation of the __stdio_common_*printf family.
 * mode: 0 = vsprintf, 1 = vsprintf_s, 2 = vsnprintf_s, 3 = vfprintf. */
static int common_printf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret,int wide,
                         int mode){
    uint32_t options,buffer=0,capacity=0,format,locale,arglist,max_count=0;
    int rc=gcrt_arg32(crt,sp,0,&options);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&buffer);
    uint32_t index;
    if(mode==3){
        /* __stdio_common_vfprintf(options, stream, format, locale, arglist) */
        capacity=UINT32_MAX;
        index=2;
    }else{
        if(!rc)rc=gcrt_arg32(crt,sp,2,&capacity);
        index=3;
        if(mode==2){
            if(!rc)rc=gcrt_arg32(crt,sp,3,&max_count);
            index=4;
        }
    }
    if(!rc)rc=gcrt_arg32(crt,sp,index,&format);
    if(!rc)rc=gcrt_arg32(crt,sp,index+1,&locale);
    if(!rc)rc=gcrt_arg32(crt,sp,index+2,&arglist);
    if(rc)return rc;
    if(locale)return gcrt_stop(crt,"__stdio_common_*printf: explicit locale object");
    GcrtVa va={crt,arglist};
    if(mode==3){
        FileObj file;
        rc=file_load(crt,buffer,&file);
        if(rc)return rc;
        (void)file;
        FileSink sink={crt,buffer,0};
        Fmt f={crt,wide,file_sink_write,&sink,0};
        rc=format_run(crt,&f,wide,format,&va);
        if(rc)return rc;
        if(sink.error)return result_eax(ret,0xffffffffu);
        return result_eax(ret,f.count);
    }
    BufSink sink={crt,buffer,0,0,wide,0};
    uint32_t limit;
    if(capacity==UINT32_MAX)limit=UINT32_MAX;
    else if(!capacity)limit=0;
    else limit=capacity-1;
    sink.capacity=limit;
    Fmt f={crt,wide,buf_sink_write,&sink,0};
    rc=format_run(crt,&f,wide,format,&va);
    if(rc)return rc;
    uint32_t count=f.count;
    if(capacity==UINT32_MAX){
        uint32_t unit_size=wide?2u:1u;
        if(buffer){
            uint8_t units[2]={0,0};
            rc=gm_write(gcrt_memory(crt),buffer+count*unit_size,units,unit_size);
            if(rc)return rc;
        }
        return result_eax(ret,count);
    }
    if(!buffer || !capacity){
        if(mode==0 && (options&GCRT_PF_LEGACY_VSPRINTF_NULL_TERMINATION))
            return result_eax(ret,count);
        return result_eax(ret,count);
    }
    uint32_t unit_size=wide?2u:1u;
    uint8_t units[2]={0,0};
    rc=gm_write(gcrt_memory(crt),buffer+sink.written*unit_size,units,unit_size);
    if(rc)return rc;
    if(mode==2 && max_count!=UINT32_MAX && count>=max_count){
        /* vsnprintf_s with a real character limit: overflow is an error. */
        uint8_t zero=0;
        rc=gm_write(gcrt_memory(crt),buffer,wide?units:&zero,unit_size);
        if(rc)return rc;
        return result_eax(ret,0xffffffffu);
    }
    int truncated=sink.written<count;
    if(!truncated)return result_eax(ret,count);
    if(mode==1){                       /* vsprintf_s: overflow is an error */
        uint8_t zero=0;
        rc=gm_write(gcrt_memory(crt),buffer,wide?units:&zero,unit_size);
        if(rc)return rc;
        return result_eax(ret,0xffffffffu);
    }
    if(mode==2)return result_eax(ret,sink.written);
    if(mode==2)return result_eax(ret,sink.written);   /* _TRUNCATE */
    return result_eax(ret,(options&GCRT_PF_STANDARD_SNPRINTF_BEHAVIOR)?count:0xffffffffu);
}

static int crt_vfprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,0,3);
}
static int crt_vfwprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,1,3);
}
static int crt_vsprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,0,0);
}
static int crt_vswprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,1,0);
}
static int crt_vsprintf_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,0,1);
}
static int crt_vswprintf_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,1,1);
}
static int crt_vsnprintf_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_printf(crt,sp,ret,0,2);
}

/* Convenience wrappers (sprintf family) with the varargs on the guest stack. */
static int crt_printf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t format,stream;
    int rc=gcrt_arg32(crt,sp,0,&format);
    if(rc)return rc;
    rc=std_file(crt,1,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    (void)file;
    FileSink sink={crt,stream,0};
    Fmt f={crt,0,file_sink_write,&sink,0};
    GcrtVa va={crt,sp+8};
    rc=format_run(crt,&f,0,format,&va);
    if(rc)return rc;
    if(sink.error)return result_eax(ret,0xffffffffu);
    return result_eax(ret,f.count);
}
static int crt_fprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream,format;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&format);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    (void)file;
    FileSink sink={crt,stream,0};
    Fmt f={crt,0,file_sink_write,&sink,0};
    GcrtVa va={crt,sp+12};
    rc=format_run(crt,&f,0,format,&va);
    if(rc)return rc;
    if(sink.error)return result_eax(ret,0xffffffffu);
    return result_eax(ret,f.count);
}
static int crt_sprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t buffer,format;
    int rc=gcrt_arg32(crt,sp,0,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&format);
    if(rc)return rc;
    BufSink sink={crt,buffer,UINT32_MAX,0,0,0};
    Fmt f={crt,0,buf_sink_write,&sink,0};
    GcrtVa va={crt,sp+12};
    rc=format_run(crt,&f,0,format,&va);
    if(rc)return rc;
    uint8_t zero=0;
    rc=gm_write(gcrt_memory(crt),buffer+f.count,&zero,1);
    if(rc)return rc;
    return result_eax(ret,f.count);
}
static int crt_snprintf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t buffer,capacity,format;
    int rc=gcrt_arg32(crt,sp,0,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&capacity);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&format);
    if(rc)return rc;
    BufSink sink={crt,buffer,capacity?capacity-1:0,0,0,0};
    Fmt f={crt,0,buf_sink_write,&sink,0};
    GcrtVa va={crt,sp+16};
    rc=format_run(crt,&f,0,format,&va);
    if(rc)return rc;
    if(capacity){
        uint8_t zero=0;
        rc=gm_write(gcrt_memory(crt),buffer+sink.written,&zero,1);
        if(rc)return rc;
    }
    return result_eax(ret,f.count);
}

static int common_scanf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret,int wide_format,
                        int from_file){
    uint32_t options,source,format,locale,arglist;
    int rc=gcrt_arg32(crt,sp,0,&options);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&source);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&format);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&locale);
    if(!rc)rc=gcrt_arg32(crt,sp,4,&arglist);
    if(rc)return rc;
    (void)options;
    if(locale)return gcrt_stop(crt,"__stdio_common_*scanf: explicit locale object");
    Scan scan;
    memset(&scan,0,sizeof(scan));
    scan.crt=crt;scan.wide_format=wide_format;scan.from_file=from_file;
    if(from_file&&wide_format)
        return gcrt_stop(crt,"__stdio_common_vfwscanf: wide stream input");
    if(from_file)scan.file=source;
    else{scan.buffer=source;scan.wide_input=wide_format;}
    GcrtVa va={crt,arglist};
    rc=scan_run(crt,&scan,format,&va);
    if(rc)return rc;
    if(scan.assigned)return result_eax(ret,scan.assigned);
    return result_eax(ret,scan.input_failure?0xffffffffu:0);
}
static int crt_vfscanf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_scanf(crt,sp,ret,0,1);
}
static int crt_vfwscanf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_scanf(crt,sp,ret,1,1);
}
static int crt_vsscanf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_scanf(crt,sp,ret,0,0);
}
static int crt_vswscanf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return common_scanf(crt,sp,ret,1,0);
}
static int crt_sscanf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t buffer,format;
    int rc=gcrt_arg32(crt,sp,0,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&format);
    if(rc)return rc;
    Scan scan;
    memset(&scan,0,sizeof(scan));
    scan.crt=crt;scan.buffer=buffer;
    GcrtVa va={crt,sp+12};
    rc=scan_run(crt,&scan,format,&va);
    if(rc)return rc;
    if(scan.assigned)return result_eax(ret,scan.assigned);
    return result_eax(ret,scan.input_failure?0xffffffffu:0);
}

/* ---- fopen/fclose and friends ------------------------------------- */

static int parse_mode(const char *mode,uint32_t *access,uint32_t *disposition,
                      uint32_t *flags){
    if(!mode || !*mode)return 0;
    char first=mode[0];
    int plus=0,binary=0;
    for(const char *p=mode+1;*p;p++){
        if(*p=='+')plus=1;
        else if(*p=='b'||*p=='t')binary=1;
        else if(*p=='x'||*p=='c'||*p=='n'||*p=='S'||*p=='R')return 0;
        else return 0;
    }
    (void)binary;
    uint32_t read=0x80000000u,write=0x40000000u;
    *flags=0;
    if(first=='r'){
        *access=read|(plus?write:0);
        *disposition=3;   /* OPEN_EXISTING */
    }else if(first=='w'){
        *access=write|(plus?read:0);
        *disposition=2;   /* CREATE_ALWAYS */
    }else if(first=='a'){
        *access=write|(plus?read:0);
        *disposition=4;   /* OPEN_ALWAYS */
        *flags|=GCRT_FILE_APPEND;
    }else return 0;
    return 1;
}
static int crt_fopen(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t path,mode_address;
    int rc=gcrt_arg32(crt,sp,0,&path);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&mode_address);
    if(rc)return rc;
    char mode[16];
    rc=gcrt_read_ansi(crt,mode_address,mode,sizeof(mode),NULL);
    if(rc)return rc;
    uint32_t access,disposition,flags;
    if(!parse_mode(mode,&access,&disposition,&flags)){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_eax(ret,0);
    }
    GuestFileApi *files=gcrt_files(crt);
    if(!files)return gcrt_stop(crt,"fopen: no file API attached");
    uint32_t handle=0;
    rc=gfa_create_file_a(gcrt_memory(crt),gcrt_teb(crt),files,path,access,3,0,
                         disposition,0x80,0,&handle);
    if(rc)return rc;
    if(handle==0xffffffffu){
        uint32_t error=0;
        if(!guest_last_error(crt,&error))gcrt_set_errno(crt,error?error:GCRT_ENOENT);
        return result_eax(ret,0);
    }
    uint32_t file_flags=0;
    if(access&0x80000000u)file_flags|=GCRT_FILE_READABLE;
    if(access&0x40000000u)file_flags|=GCRT_FILE_WRITABLE;
    file_flags|=flags;
    uint32_t address=0;
    rc=file_create(crt,handle,file_flags,&address);
    if(rc)return rc;
    if(file_flags&GCRT_FILE_APPEND){
        FileObj file;
        rc=file_load(crt,address,&file);
        if(rc)return rc;
        uint32_t position=0;
        rc=file_seek(crt,&file,0,2,&position);
        if(rc)return rc;
        rc=file_store(crt,address,&file);
        if(rc)return rc;
    }
    ret->eax=address;return GM_OK;
}
static int crt_fclose(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,address,&file);
    if(rc)return rc;
    if(file.flags&GCRT_FILE_CONSOLE){
        /* Closing a standard stream is allowed; the object stays (like the
         * UCRT's static stream array) but loses its direction. */
        file.flags&=~(GCRT_FILE_READABLE|GCRT_FILE_WRITABLE);
        rc=file_store(crt,address,&file);
        if(rc)return rc;
        return result_eax(ret,0);
    }
    uint32_t result=0;
    rc=file_close(crt,&file,&result);
    if(rc)return rc;
    if(!result){
        gcrt_set_errno(crt,GCRT_EIO);
        return result_eax(ret,0xffffffffu);
    }
    uint8_t zero[GCRT_FILE_SIZE];
    memset(zero,0,sizeof(zero));
    rc=gm_write(gcrt_memory(crt),address,zero,GCRT_FILE_SIZE);   /* invalidate */
    if(rc)return rc;
    return result_eax(ret,0);
}
static int crt_fread(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t buffer,size,count,stream;
    int rc=gcrt_arg32(crt,sp,0,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&size);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    if(!size||!count)return result_eax(ret,0);
    uint64_t total=(uint64_t)size*count;
    if(total>UINT32_MAX)return gcrt_stop(crt,"fread: request larger than 4 GiB");
    uint8_t *bytes=malloc((size_t)total);
    if(!bytes)return GM_ALLOCATION;
    uint32_t done=0;
    rc=file_read_host(crt,&file,bytes,(uint32_t)total,&done);
    if(!rc && done)rc=gm_write(gcrt_memory(crt),buffer,bytes,done);
    rc=rc ? rc : file_store(crt,stream,&file);
    free(bytes);
    if(rc)return rc;
    return result_eax(ret,done/size);
}
static int crt_fwrite(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t buffer,size,count,stream;
    int rc=gcrt_arg32(crt,sp,0,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&size);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    if(!size||!count)return result_eax(ret,0);
    uint64_t total=(uint64_t)size*count;
    if(total>UINT32_MAX)return gcrt_stop(crt,"fwrite: request larger than 4 GiB");
    uint8_t *bytes=malloc((size_t)total);
    if(!bytes)return GM_ALLOCATION;
    rc=gm_read(gcrt_memory(crt),buffer,bytes,(uint32_t)total);
    uint32_t done=0;
    if(!rc)rc=file_write_host(crt,&file,bytes,(uint32_t)total,&done);
    if(!rc)rc=file_store(crt,stream,&file);
    free(bytes);
    if(rc)return rc;
    return result_eax(ret,done/size);
}
static int crt_fseek(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream,offset,origin;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&offset);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&origin);
    if(rc)return rc;
    if(origin>2){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_eax(ret,0xffffffffu);
    }
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    uint32_t position=0;
    rc=file_seek(crt,&file,(int64_t)(int32_t)offset,origin,&position);
    if(rc)return rc;
    rc=file_store(crt,stream,&file);
    if(rc)return rc;
    if(!position && (file.flags&GCRT_FILE_ERROR)){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_eax(ret,0xffffffffu);
    }
    return result_eax(ret,0);
}
static int crt_ftell(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    uint32_t position=0;
    rc=file_seek(crt,&file,0,1,&position);
    if(rc)return rc;
    rc=file_store(crt,stream,&file);
    if(rc)return rc;
    if(!position && (file.flags&GCRT_FILE_ERROR)){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_eax(ret,0xffffffffu);
    }
    return result_eax(ret,position);
}
static int crt_feof(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    return result_eax(ret,(file.flags&GCRT_FILE_EOF)?1u:0u);
}
static int crt_ferror(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    return result_eax(ret,(file.flags&GCRT_FILE_ERROR)?1u:0u);
}
static int crt_fflush(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(rc)return rc;
    if(!stream)return result_eax(ret,0);   /* flush all: nothing is buffered */
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    uint32_t result=0;
    rc=file_flush(crt,&file,&result);
    if(rc)return rc;
    rc=file_store(crt,stream,&file);
    if(rc)return rc;
    return result_eax(ret,result?0u:0xffffffffu);
}
static int crt_fputs(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t text,stream;
    int rc=gcrt_arg32(crt,sp,0,&text);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&stream);
    if(rc)return rc;
    uint32_t length=0;
    rc=gcrt_strlen_ansi(crt,text,0x100000u,&length);
    if(rc)return rc;
    char *bytes=malloc(length+1);
    if(!bytes)return GM_ALLOCATION;
    rc=gm_read(gcrt_memory(crt),text,bytes,length+1);
    if(rc){free(bytes);return rc;}
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc){free(bytes);return rc;}
    uint32_t done=0;
    rc=file_write_host(crt,&file,bytes,length,&done);
    if(!rc)rc=file_store(crt,stream,&file);
    free(bytes);
    if(rc)return rc;
    if(done!=length){
        gcrt_set_errno(crt,GCRT_EIO);
        return result_eax(ret,0xffffffffu);
    }
    return result_eax(ret,0);
}
static int crt_getc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    uint8_t byte=0;
    uint32_t done=0;
    rc=file_read_host(crt,&file,&byte,1,&done);
    if(!rc)rc=file_store(crt,stream,&file);
    if(rc)return rc;
    if(!done)return result_eax(ret,0xffffffffu);
    return result_eax(ret,byte);
}
static int crt_ungetc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t value,stream;
    int rc=gcrt_arg32(crt,sp,0,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&stream);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    if(file.pushback!=UINT32_MAX){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_eax(ret,0xffffffffu);
    }
    file.pushback=value&0xff;
    file.flags&=~(GCRT_FILE_EOF|GCRT_FILE_ERROR);
    rc=file_store(crt,stream,&file);
    if(rc)return rc;
    return result_eax(ret,value&0xff);
}
static int crt_setvbuf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t stream,buffer,mode,size;
    int rc=gcrt_arg32(crt,sp,0,&stream);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&mode);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&size);
    if(rc)return rc;
    FileObj file;
    rc=file_load(crt,stream,&file);
    if(rc)return rc;
    if(mode>2){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_eax(ret,0xffffffffu);
    }
    /* No buffering is modelled: every write reaches the file API directly. */
    return result_eax(ret,0);
}
static int crt_tmpfile(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;
    GuestFileApi *files=gcrt_files(crt);
    if(!files)return gcrt_stop(crt,"tmpfile: no file API attached");
    uint32_t *counter=gcrt_tmpfile_counter(crt);
    if(!counter)return GM_RANGE;
    char path[128];
    int rc;
    snprintf(path,sizeof(path),
             "C:\\Users\\Player\\AppData\\Local\\crt%08x.tmp",(*counter)++);
    uint32_t handle=0;
    /* gfa_create_file_a needs the guest address of the name, so place it. */
    uint32_t buffer=0;
    rc=gcrt_scratch(crt,GCRT_SLOT_IOBUF,1024,&buffer);
    if(rc)return rc;
    rc=gcrt_write_ansi(crt,buffer,path,1024);
    if(rc)return rc;
    rc=gfa_create_file_a(gcrt_memory(crt),gcrt_teb(crt),files,buffer,
                         0x80000000u|0x40000000u,3,0,2,0x80,0,&handle);
    if(rc)return rc;
    if(handle==0xffffffffu){
        gcrt_set_errno(crt,GCRT_EIO);
        return result_eax(ret,0);
    }
    uint32_t address=0;
    rc=file_create(crt,handle,GCRT_FILE_READABLE|GCRT_FILE_WRITABLE,&address);
    if(rc)return rc;
    ret->eax=address;return GM_OK;
}
static int crt_popen(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return gcrt_stop(crt,"_popen: process creation is not modelled");
}
static int crt_pclose(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return gcrt_stop(crt,"_pclose: process creation is not modelled");
}

#define STDIO_DLL "api-ms-win-crt-stdio-l1-1-0.dll"
const GcrtExport crt_stdio_exports[]={
    /* user32!wsprintfA: cdecl varargs, identical (buf,fmt,...) layout to sprintf,
     * ANSI, no float specifiers -- routed to the CRT formatter. SB_CRT cdecl
     * cleanup (caller pops) matches wsprintf's calling convention. */
    {"user32.dll","wsprintfA",0,crt_sprintf},
    {STDIO_DLL,"__acrt_iob_func",0,crt_acrt_iob_func},
    {STDIO_DLL,"__stdio_common_vfprintf",0,crt_vfprintf},
    {STDIO_DLL,"__stdio_common_vfwprintf",0,crt_vfwprintf},
    {STDIO_DLL,"__stdio_common_vfscanf",0,crt_vfscanf},
    {STDIO_DLL,"__stdio_common_vfwscanf",0,crt_vfwscanf},
    {STDIO_DLL,"__stdio_common_vsnprintf_s",0,crt_vsnprintf_s},
    {STDIO_DLL,"__stdio_common_vsprintf",0,crt_vsprintf},
    {STDIO_DLL,"__stdio_common_vsprintf_s",0,crt_vsprintf_s},
    {STDIO_DLL,"__stdio_common_vsscanf",0,crt_vsscanf},
    {STDIO_DLL,"__stdio_common_vswprintf",0,crt_vswprintf},
    {STDIO_DLL,"__stdio_common_vswprintf_s",0,crt_vswprintf_s},
    {STDIO_DLL,"__stdio_common_vswscanf",0,crt_vswscanf},
    {STDIO_DLL,"_pclose",0,crt_pclose},
    {STDIO_DLL,"_popen",0,crt_popen},
    {STDIO_DLL,"fclose",0,crt_fclose},
    {STDIO_DLL,"feof",0,crt_feof},
    {STDIO_DLL,"ferror",0,crt_ferror},
    {STDIO_DLL,"fflush",0,crt_fflush},
    {STDIO_DLL,"fopen",0,crt_fopen},
    {STDIO_DLL,"fputs",0,crt_fputs},
    {STDIO_DLL,"fread",0,crt_fread},
    {STDIO_DLL,"fseek",0,crt_fseek},
    {STDIO_DLL,"ftell",0,crt_ftell},
    {STDIO_DLL,"fwrite",0,crt_fwrite},
    {STDIO_DLL,"getc",0,crt_getc},
    {STDIO_DLL,"setvbuf",0,crt_setvbuf},
    {STDIO_DLL,"tmpfile",0,crt_tmpfile},
    {STDIO_DLL,"ungetc",0,crt_ungetc},
    {STDIO_DLL,"printf",0,crt_printf},
    {STDIO_DLL,"fprintf",0,crt_fprintf},
    {STDIO_DLL,"sprintf",0,crt_sprintf},
    {STDIO_DLL,"snprintf",0,crt_snprintf},
    {STDIO_DLL,"sscanf",0,crt_sscanf}
};
const uint32_t crt_stdio_export_count=sizeof(crt_stdio_exports)/sizeof(crt_stdio_exports[0]);
