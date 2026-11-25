/* UCRT conversion bridges (api-ms-win-crt-convert-l1-1-0.dll).
 *
 * atoi/atof/strtol/strtoul/_strtoi64/_strtoui64/strtod parse guest strings with
 * the documented UCRT grammar (leading isspace, optional sign, base prefixes
 * 0x/0 for strtol base 0, decimal and hexadecimal floating point, inf/nan) and
 * report the stop position through the guest endptr.
 *
 * _itoa/_itoa_s/_ecvt/_fcvt write into guest buffers; _ecvt/_fcvt use a
 * per-context guest digit buffer (GCRT_SLOT_ECVT) like the UCRT per-thread
 * static buffer.
 *
 * wcstombs_s converts UTF-16 to the installed ANSI code page (CP1252) through
 * the GuestLocale; unmappable code points are EILSEQ, never a silent zero.
 *
 * Documented limits: long is 32 bits (Win32), so strtol/strtoul saturate at
 * 0x7fffffff/0x80000000/0xffffffff and set errno=ERANGE; _strtoi64 and
 * _strtoui64 saturate at the 64-bit limits. No locale-dependent decimal point
 * (the C locale uses '.'). */
#include "guest_crt.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GCRT_MAX_NUMBER 512u

static int write_u32(GuestCrt *crt,uint32_t address,uint32_t value){
    if(!address)return GM_OK;
    uint8_t bytes[4]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16),
                      (uint8_t)(value>>24)};
    return gm_write(gcrt_memory(crt),address,bytes,4);
}

/* ------------------------------------------------------------------ */
/* Integer parsing                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int negative,digits,overflow,base;
    uint64_t magnitude;
    uint32_t end;
} Parsed;

static int parse_integer(GuestCrt *crt,uint32_t address,int base,Parsed *out){
    if(!crt || !out)return GM_RANGE;
    memset(out,0,sizeof(*out));
    out->end=address;
    if(!address)return GM_OK;
    uint32_t position=address;
    int rc;
    for(;;){                                  /* leading whitespace */
        uint8_t byte;
        rc=gm_read(gcrt_memory(crt),position,&byte,1);
        if(rc)return rc;
        if(!byte)return GM_OK;
        if(!(byte==' '||(byte>=0x09&&byte<=0x0d)))break;
        position++;
    }
    uint8_t byte;
    rc=gm_read(gcrt_memory(crt),position,&byte,1);
    if(rc)return rc;
    if(byte=='+'||byte=='-'){
        out->negative=byte=='-';
        position++;
        rc=gm_read(gcrt_memory(crt),position,&byte,1);
        if(rc)return rc;
    }
    int detected=base;
    if(detected==0){
        detected=10;
        if(byte=='0'){
            position++;
            rc=gm_read(gcrt_memory(crt),position,&byte,1);
            if(rc)return rc;
            if(byte=='x'||byte=='X'){
                detected=16;
                position++;
                rc=gm_read(gcrt_memory(crt),position,&byte,1);
                if(rc)return rc;
            }else detected=8;
        }
    }else if(detected==16 && byte=='0'){
        uint8_t next;
        rc=gm_read(gcrt_memory(crt),position+1,&next,1);
        if(rc)return rc;
        if(next=='x'||next=='X'){
            position+=2;
            rc=gm_read(gcrt_memory(crt),position,&byte,1);
            if(rc)return rc;
        }
    }
    out->base=detected;
    for(;;){
        int value;
        if(byte>='0'&&byte<='9')value=byte-'0';
        else if(byte>='a'&&byte<='z')value=byte-'a'+10;
        else if(byte>='A'&&byte<='Z')value=byte-'A'+10;
        else break;
        if(value>=detected)break;
        if(out->magnitude>(UINT64_MAX-(uint64_t)value)/(uint64_t)detected)
            out->overflow=1;
        else out->magnitude=out->magnitude*(uint64_t)detected+(uint64_t)value;
        out->digits++;
        position++;
        rc=gm_read(gcrt_memory(crt),position,&byte,1);
        if(rc)return rc;
    }
    out->end=position;
    return GM_OK;
}

static int convert_strtol(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,endptr,base;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&endptr);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&base);
    if(rc)return rc;
    if(base!=0 && (base<2 || base>36))return gcrt_stop(crt,"strtol: base out of range");
    Parsed parsed;
    rc=parse_integer(crt,address,(int)base,&parsed);
    if(rc)return rc;
    if(endptr)rc=write_u32(crt,endptr,parsed.end);
    if(rc)return rc;
    if(!parsed.digits)return (ret->eax=0),GM_OK;
    int64_t limit=INT32_MAX;
    int64_t low=INT32_MIN;
    if(parsed.overflow || parsed.magnitude>(uint64_t)limit){
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=parsed.negative ? (uint32_t)low : (uint32_t)limit;
        return GM_OK;
    }
    int64_t value=parsed.negative ? -(int64_t)parsed.magnitude : (int64_t)parsed.magnitude;
    ret->eax=(uint32_t)value;
    return GM_OK;
}
static int convert_strtoul(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,endptr,base;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&endptr);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&base);
    if(rc)return rc;
    if(base!=0 && (base<2 || base>36))return gcrt_stop(crt,"strtoul: base out of range");
    Parsed parsed;
    rc=parse_integer(crt,address,(int)base,&parsed);
    if(rc)return rc;
    if(endptr)rc=write_u32(crt,endptr,parsed.end);
    if(rc)return rc;
    if(!parsed.digits)return (ret->eax=0),GM_OK;
    if(parsed.overflow || parsed.magnitude>UINT32_MAX){
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=UINT32_MAX;
        return GM_OK;
    }
    uint32_t value=(uint32_t)parsed.magnitude;
    if(parsed.negative)value=(uint32_t)(0u-value);
    ret->eax=value;
    return GM_OK;
}
static int convert_strtoi64_common(GuestCrt *crt,uint32_t sp,GcrtReturn *ret,int is_signed){
    uint32_t address,endptr,base;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&endptr);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&base);
    if(rc)return rc;
    if(base!=0 && (base<2 || base>36))return gcrt_stop(crt,"_strtoi64: base out of range");
    Parsed parsed;
    rc=parse_integer(crt,address,(int)base,&parsed);
    if(rc)return rc;
    if(endptr)rc=write_u32(crt,endptr,parsed.end);
    if(rc)return rc;
    ret->edx_valid=1;
    if(!parsed.digits){ret->eax=0;ret->edx=0;return GM_OK;}
    if(is_signed){
        if(parsed.overflow || parsed.magnitude>UINT64_C(0x8000000000000000)){
            gcrt_set_errno(crt,GCRT_ERANGE);
            uint64_t limit=parsed.negative ? UINT64_C(0x8000000000000000)
                                           : UINT64_C(0x7fffffffffffffff);
            ret->eax=(uint32_t)limit;ret->edx=(uint32_t)(limit>>32);return GM_OK;
        }
        int64_t value=parsed.negative ? -(int64_t)parsed.magnitude
                                      : (int64_t)parsed.magnitude;
        ret->eax=(uint32_t)(uint64_t)value;ret->edx=(uint32_t)((uint64_t)value>>32);
        return GM_OK;
    }
    if(parsed.overflow){
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=UINT32_MAX;ret->edx=UINT32_MAX;return GM_OK;
    }
    uint64_t value=parsed.negative ? (uint64_t)(0-parsed.magnitude) : parsed.magnitude;
    ret->eax=(uint32_t)value;ret->edx=(uint32_t)(value>>32);
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* Floating point parsing                                               */
/* ------------------------------------------------------------------ */

static int convert_strtod(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,endptr;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&endptr);
    if(rc)return rc;
    uint32_t end=address;
    double value=0.0;
    if(address){
        /* Collect the candidate token; the host strtod decides where the
         * number really ends, then the unconsumed tail is not consumed. */
        char token[GCRT_MAX_NUMBER];
        uint32_t length=0,start=address;
        for(;;){
            uint8_t byte;
            rc=gm_read(gcrt_memory(crt),start+length,&byte,1);
            if(rc)return rc;
            if(!byte)break;
            if(length+1>=sizeof(token))break;
            token[length++]=(char)byte;
        }
        token[length]=0;
        char *stop=NULL;
        value=strtod(token,&stop);
        uint32_t consumed=stop ? (uint32_t)(stop-token) : 0;
        end=address+consumed;
        if(errno==ERANGE)gcrt_set_errno(crt,GCRT_ERANGE);
        errno=0;
    }
    if(endptr)rc=write_u32(crt,endptr,end);
    if(rc)return rc;
    uint64_t bits;
    memcpy(&bits,&value,8);
    ret->st0_bits=bits;ret->st0_valid=1;
    return GM_OK;
}
static int convert_atof(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    /* atof(s) == strtod(s, NULL) */
    double value=0.0;
    if(address){
        char token[GCRT_MAX_NUMBER];
        uint32_t length=0;
        for(;;){
            uint8_t byte;
            rc=gm_read(gcrt_memory(crt),address+length,&byte,1);
            if(rc)return rc;
            if(!byte)break;
            if(length+1>=sizeof(token))break;
            token[length++]=(char)byte;
        }
        token[length]=0;
        value=strtod(token,NULL);
        errno=0;
    }
    uint64_t bits;
    memcpy(&bits,&value,8);
    ret->st0_bits=bits;ret->st0_valid=1;
    return GM_OK;
}
static int convert_atoi(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    Parsed parsed;
    rc=parse_integer(crt,address,10,&parsed);
    if(rc)return rc;
    if(!parsed.digits){ret->eax=0;return GM_OK;}
    if(parsed.overflow)gcrt_set_errno(crt,GCRT_ERANGE);
    int64_t value=parsed.negative ? -(int64_t)parsed.magnitude : (int64_t)parsed.magnitude;
    ret->eax=(uint32_t)value;
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* _itoa                                                                */
/* ------------------------------------------------------------------ */

static uint32_t itoa_digits(char *out,uint32_t value,int radix,int negative){
    char reversed[40];
    uint32_t count=0;
    if(!value)reversed[count++]='0';
    while(value){
        uint32_t digit=value%(uint32_t)radix;
        reversed[count++]=(char)(digit<10 ? '0'+digit : 'a'+digit-10);
        value/=(uint32_t)radix;
    }
    uint32_t length=0;
    if(negative)out[length++]='-';
    while(count)out[length++]=reversed[--count];
    out[length]=0;
    return length;
}
static int convert_itoa(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t value,buffer,radix;
    int rc=gcrt_arg32(crt,sp,0,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&radix);
    if(rc)return rc;
    if(radix<2||radix>36)return gcrt_stop(crt,"_itoa: radix out of range");
    if(!buffer)return gcrt_stop(crt,"_itoa: NULL buffer");
    int negative=(int32_t)value<0 && radix==10;
    char text[40];
    itoa_digits(text,negative?(uint32_t)(-(int32_t)value):value,(int)radix,negative);
    rc=gcrt_write_ansi(crt,buffer,text,sizeof(text));
    if(rc)return rc;
    ret->eax=buffer;return GM_OK;
}
static int convert_itoa_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t value,buffer,size,radix;
    int rc=gcrt_arg32(crt,sp,0,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&buffer);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&size);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&radix);
    if(rc)return rc;
    if(!buffer || !size || radix<2 || radix>36){
        gcrt_set_errno(crt,GCRT_EINVAL);
        ret->eax=GCRT_EINVAL;return GM_OK;
    }
    int negative=(int32_t)value<0 && radix==10;
    char text[40];
    uint32_t length=itoa_digits(text,negative?(uint32_t)(-(int32_t)value):value,
                                (int)radix,negative);
    if(length+1>size){
        uint8_t zero=0;
        rc=gm_write(gcrt_memory(crt),buffer,&zero,1);
        if(rc)return rc;
        gcrt_set_errno(crt,GCRT_ERANGE);
        ret->eax=GCRT_ERANGE;return GM_OK;
    }
    rc=gcrt_write_ansi(crt,buffer,text,size);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}

/* ------------------------------------------------------------------ */
/* _ecvt / _fcvt                                                        */
/* ------------------------------------------------------------------ */

static int convert_ecvt_common(GuestCrt *crt,uint32_t sp,GcrtReturn *ret,int fixed){
    uint32_t value_low,value_high,count,decpt,sign;
    int rc=gcrt_arg32(crt,sp,0,&value_low);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&value_high);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&decpt);
    if(!rc)rc=gcrt_arg32(crt,sp,4,&sign);
    if(rc)return rc;
    uint64_t bits=((uint64_t)value_high<<32)|value_low;
    double value;
    memcpy(&value,&bits,8);
    if(!isfinite(value))return gcrt_stop(crt,"_ecvt/_fcvt: non-finite value");
    int negative=signbit(value)!=0;
    double magnitude=negative ? -value : value;
    if(count>512)return gcrt_stop(crt,"_ecvt/_fcvt: count above 512");
    uint32_t buffer=0;
    rc=gcrt_scratch(crt,GCRT_SLOT_ECVT,1024,&buffer);
    if(rc)return rc;
    char text[1024];
    int length=0;
    int exponent=0;
    if(fixed){
        length=snprintf(text,sizeof(text),"%.*f",(int)count,magnitude);
        if(length<0)return gcrt_stop(crt,"_fcvt: host conversion failed");
        if((size_t)length>=sizeof(text))length=(int)sizeof(text)-1;
        /* decpt is the number of digits before the decimal point; _fcvt
         * returns the `count` digits after it. */
        char *point=strchr(text,'.');
        int point_index=point ? (int)(point-text) : length;
        exponent=point_index;
        /* A single leading "0" before the point means zero integer digits. */
        if(exponent==1 && text[0]=='0')exponent=0;
        char digits[1024];
        int n=0;
        if(point)for(int i=point_index+1;i<length;i++)digits[n++]=text[i];
        while(n<(int)count)digits[n++]='0';
        memcpy(text,digits,(size_t)n);
        length=n;
    }else{
        length=snprintf(text,sizeof(text),"%.*e",(int)(count?count-1:0),magnitude);
        if(length<0)return gcrt_stop(crt,"_ecvt: host conversion failed");
        if((size_t)length>=sizeof(text))length=(int)sizeof(text)-1;
        char *marker=strchr(text,'e');
        exponent=marker ? atoi(marker+1) : 0;
        char digits[1024];
        int n=0;
        for(int i=0;i<length;i++){
            if(text[i]=='e'||text[i]=='E')break;
            if(text[i]!='.')digits[n++]=text[i];
        }
        while(n<(int)count)digits[n++]='0';
        memcpy(text,digits,(size_t)n);
        length=n;
    }
    if(!count){
        text[0]=0;length=0;
        if(!fixed)exponent=0;
    }
    if((uint32_t)length>count)length=(int)count;
    text[length]=0;
    rc=gcrt_write_ansi(crt,buffer,text,1024);
    if(rc)return rc;
    if(decpt)rc=write_u32(crt,decpt,(uint32_t)(fixed ? exponent : exponent+1));
    if(!rc && sign)rc=write_u32(crt,sign,negative?1u:0u);
    if(rc)return rc;
    ret->eax=buffer;return GM_OK;
}
static int convert_ecvt(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return convert_ecvt_common(crt,sp,ret,0);
}
static int convert_fcvt(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return convert_ecvt_common(crt,sp,ret,1);
}

/* ------------------------------------------------------------------ */
/* wcstombs_s                                                           */
/* ------------------------------------------------------------------ */

static int convert_wcstombs_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t returned,mbstr,size,wcstr,count;
    int rc=gcrt_arg32(crt,sp,0,&returned);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&mbstr);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&size);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&wcstr);
    if(!rc)rc=gcrt_arg32(crt,sp,4,&count);
    if(rc)return rc;
    if(!returned || (!mbstr && size) || !wcstr){
        gcrt_set_errno(crt,GCRT_EINVAL);
        ret->eax=GCRT_EINVAL;return GM_OK;
    }
    const GuestLocale *locale=gcrt_locale(crt);
    if(!locale || !locale->installed)
        return gcrt_stop(crt,"wcstombs_s: no installed code page");
    uint32_t units=0;
    if(count==UINT32_MAX){
        rc=gcrt_strlen_wide(crt,wcstr,0x100000u,&units);
        if(rc)return rc;
    }else units=count;
    uint8_t *bytes=malloc(units+1);
    if(!bytes)return GM_ALLOCATION;
    int result=0;
    uint32_t produced=0;
    for(uint32_t i=0;i<units;i++){
        uint8_t raw[2];
        rc=gm_read(gcrt_memory(crt),wcstr+i*2,raw,2);
        if(rc){free(bytes);return rc;}
        uint16_t unit=(uint16_t)(raw[0]|(raw[1]<<8));
        if(!unit)break;
        if(unit>=0xd800&&unit<=0xdfff){
            free(bytes);
            gcrt_set_errno(crt,GCRT_EILSEQ);
            ret->eax=GCRT_EILSEQ;return GM_OK;
        }
        uint8_t byte=locale->encode[unit];
        /* The frozen NLS uni2cp table maps unmappable code points to '?', so a
         * round trip through decode[] is the only reliable mappability test. */
        if(locale->decode[byte]!=unit){
            free(bytes);
            if(mbstr&&size){
                uint8_t zero=0;
                rc=gm_write(gcrt_memory(crt),mbstr,&zero,1);
                if(rc)return rc;
            }
            gcrt_set_errno(crt,GCRT_EILSEQ);
            ret->eax=GCRT_EILSEQ;return GM_OK;
        }
        bytes[produced++]=byte;
    }
    uint32_t needed=produced+1;
    if(size && needed>size){
        if(count==UINT32_MAX){
            produced=size-1;
            needed=size;
        }else{
            free(bytes);
            if(size){
                uint8_t zero=0;
                rc=gm_write(gcrt_memory(crt),mbstr,&zero,1);
                if(rc)return rc;
            }
            gcrt_set_errno(crt,GCRT_ERANGE);
            ret->eax=GCRT_ERANGE;return GM_OK;
        }
    }
    if(mbstr && size){
        rc=gm_write(gcrt_memory(crt),mbstr,bytes,produced);
        if(!rc){
            uint8_t zero=0;
            rc=gm_write(gcrt_memory(crt),mbstr+produced,&zero,1);
        }
        if(rc){free(bytes);return rc;}
    }
    free(bytes);
    rc=write_u32(crt,returned,needed);
    if(rc)return rc;
    ret->eax=(uint32_t)result;
    return GM_OK;
}

static int convert_strtoi64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return convert_strtoi64_common(crt,sp,ret,1);
}
static int convert_strtoui64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return convert_strtoi64_common(crt,sp,ret,0);
}

#define CONVERT_DLL "api-ms-win-crt-convert-l1-1-0.dll"
const GcrtExport crt_convert_exports[]={
    {CONVERT_DLL,"_ecvt",0,convert_ecvt},
    {CONVERT_DLL,"_fcvt",0,convert_fcvt},
    {CONVERT_DLL,"_itoa",0,convert_itoa},
    {CONVERT_DLL,"_itoa_s",0,convert_itoa_s},
    {CONVERT_DLL,"_strtoi64",0,convert_strtoi64},
    {CONVERT_DLL,"_strtoui64",0,convert_strtoui64},
    {CONVERT_DLL,"atof",0,convert_atof},
    {CONVERT_DLL,"atoi",0,convert_atoi},
    {CONVERT_DLL,"strtod",0,convert_strtod},
    {CONVERT_DLL,"strtol",0,convert_strtol},
    {CONVERT_DLL,"strtoul",0,convert_strtoul},
    {CONVERT_DLL,"wcstombs_s",0,convert_wcstombs_s}
};
const uint32_t crt_convert_export_count=sizeof(crt_convert_exports)/sizeof(crt_convert_exports[0]);
