/* UCRT misc bridges: runtime, time, utility, math, filesystem and multibyte.
 *
 *   api-ms-win-crt-runtime-l1-1-0.dll  _errno, _invalid_parameter_noinfo,
 *       _invalid_parameter_noinfo_noreturn, _resetstkoflw, exit, strerror,
 *       _initterm, _initterm_e
 *   api-ms-win-crt-time-l1-1-0.dll     __timezone, _gmtime64, _localtime64,
 *       _localtime64_s, _mktime64, _time64
 *   api-ms-win-crt-utility-l1-1-0.dll  qsort, bsearch, rand, srand
 *   api-ms-win-crt-math-l1-1-0.dll     frexp, ldexp, modf
 *   api-ms-win-crt-filesystem-l1-1-0.dll  _stat64, _wsplitpath_s
 *
 * qsort/bsearch/_initterm/_initterm_e call back into the guest through the
 * GcrtGuestCall hook; without a hook they are named stops, never a fabricated
 * ordering or initializer result.
 *
 * rand()/srand() are the MSVC linear congruential generator
 * (state = state*214013+2531011; result = (state>>16)&0x7fff, seed 1), so a
 * sequence matches the original CRT bit for bit.
 *
 * The time functions use the injected clock (gcrt_set_clock) or the host
 * CLOCK_REALTIME, and the configured Windows time zone bias. struct tm is the
 * nine-int PE32 layout (36 bytes); __timezone is a data import and therefore a
 * named stop, never a fabricated variable.
 *
 * Math results are returned as IEEE-754 binary64 in GcrtReturn.st0_bits because
 * the 32-bit cdecl ABI returns doubles on the x87 stack; see guest_crt.h for
 * how the dispatcher pushes ST0. */
#include "guest_crt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Guest callback                                                       */
/* ------------------------------------------------------------------ */

static int guest_call(GuestCrt *crt,uint32_t address,const uint32_t *args,
                      uint32_t argc,uint32_t *eax){
    void *user=NULL;
    GcrtGuestCall call=gcrt_guest_call(crt,&user);
    if(!call)return gcrt_stop(crt,"crt: guest callback hook not installed");
    return call(user,address,args,argc,eax);
}

/* ------------------------------------------------------------------ */
/* runtime                                                             */
/* ------------------------------------------------------------------ */

static int crt_errno_fn(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;
    uint32_t address=0;
    int rc=gcrt_errno_address(crt,&address);
    if(rc)return rc;
    ret->eax=address;return GM_OK;
}
static int crt_invalid_parameter_noinfo(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;
    /* No handler is installed: the default handler does nothing, errno=EINVAL. */
    int rc=gcrt_set_errno(crt,GCRT_EINVAL);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}
static int crt_invalid_parameter_noreturn(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return gcrt_stop(crt,"_invalid_parameter_noinfo_noreturn: would terminate");
}
static int crt_resetstkoflw(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;
    /* The port has no Windows stack guard page to reset; UCRT returns 1. */
    (void)crt;
    ret->eax=1;return GM_OK;
}
static int crt_exit(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return gcrt_stop(crt,"exit: guest process termination is not modelled by the CRT");
}
static int crt_initterm(GuestCrt *crt,uint32_t sp,GcrtReturn *ret,int wants_result){
    uint32_t start,end;
    int rc=gcrt_arg32(crt,sp,0,&start);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&end);
    if(rc)return rc;
    if(end<start || (end-start)%4)return gcrt_stop(crt,"_initterm: bad function range");
    uint32_t count=(end-start)/4;
    if(count>4096)return gcrt_stop(crt,"_initterm: more than 4096 initializers");
    for(uint32_t i=0;i<count;i++){
        uint8_t bytes[4];
        rc=gm_read(gcrt_memory(crt),start+i*4,bytes,4);
        if(rc)return rc;
        uint32_t function=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|
                          ((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
        if(!function)continue;
        uint32_t eax=0;
        rc=guest_call(crt,function,NULL,0,&eax);
        if(rc)return rc;
        if(wants_result && eax){ret->eax=eax;return GM_OK;}
    }
    ret->eax=0;return GM_OK;
}
static int crt_initterm_plain(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return crt_initterm(crt,sp,ret,0);
}
static int crt_initterm_e(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    return crt_initterm(crt,sp,ret,1);
}

static const char *const strerror_text[]={
    /* 0 */ "No error",
    /* 1 */ "Permission denied",
    /* 2 */ "No such file or directory",
    /* 3 */ "No such process",
    /* 4 */ "Interrupted function call",
    /* 5 */ "Input/output error",
    /* 6 */ "No such device or address",
    /* 7 */ "Arg list too long",
    /* 8 */ "Exec format error",
    /* 9 */ "Bad file descriptor",
    /* 10 */ "No child processes",
    /* 11 */ "Resource temporarily unavailable",
    /* 12 */ "Not enough space",
    /* 13 */ "Permission denied",
    /* 14 */ "Bad address",
    /* 15 */ "Unknown error",
    /* 16 */ "Resource device",
    /* 17 */ "File exists",
    /* 18 */ "Improper link",
    /* 19 */ "No such device",
    /* 20 */ "Not a directory",
    /* 21 */ "Is a directory",
    /* 22 */ "Invalid argument",
    /* 23 */ "Too many open files in system",
    /* 24 */ "Too many open files",
    /* 25 */ "Inappropriate I/O control operation",
    /* 26 */ "Unknown error",
    /* 27 */ "File too large",
    /* 28 */ "No space left on device",
    /* 29 */ "Invalid seek",
    /* 30 */ "Read-only file system",
    /* 31 */ "Too many links",
    /* 32 */ "Broken pipe",
    /* 33 */ "Domain error",
    /* 34 */ "Result too large",
    /* 35 */ "Unknown error",
    /* 36 */ "Resource deadlock avoided",
    /* 37 */ "Unknown error",
    /* 38 */ "Filename too long",
    /* 39 */ "No locks available",
    /* 40 */ "Function not implemented",
    /* 41 */ "Directory not empty",
    /* 42 */ "Illegal byte sequence"
};
static int crt_strerror(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t number;
    int rc=gcrt_arg32(crt,sp,0,&number);
    if(rc)return rc;
    const char *text="Unknown error";
    if(number<sizeof(strerror_text)/sizeof(strerror_text[0]))
        text=strerror_text[number];
    uint32_t buffer=0;
    rc=gcrt_scratch(crt,GCRT_SLOT_STRERROR,256,&buffer);
    if(rc)return rc;
    rc=gcrt_write_ansi(crt,buffer,text,256);
    if(rc)return rc;
    ret->eax=buffer;return GM_OK;
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t sec,min,hour,mday,mon,year,wday,yday,isdst;
} GuestTm;

/* Howard Hinnant's civil calendar algorithms, exact for the whole int64 range
 * we can reach from a FILETIME or a 32-bit time_t. */
static int64_t days_from_civil(int64_t y,unsigned m,unsigned d){
    y-=m<=2;
    int64_t era=(y>=0?y:y-399)/400;
    unsigned yoe=(unsigned)(y-era*400);
    unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    unsigned doe=yoe*365+yoe/4-yoe/100+doy;
    return era*146097+(int64_t)doe-719468;
}
static void civil_from_days(int64_t z,int64_t *y,unsigned *m,unsigned *d){
    z+=719468;
    int64_t era=(z>=0?z:z-146096)/146097;
    unsigned doe=(unsigned)(z-era*146097);
    unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    int64_t year=(int64_t)yoe+era*400;
    unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    unsigned mp=(5*doy+2)/153;
    unsigned day=doy-(153*mp+2)/5+1;
    unsigned month=mp+(mp<10?3:-9);
    *y=year+(month<=2);
    *m=month;*d=day;
}
static void tm_from_seconds(int64_t seconds,GuestTm *tm){
    int64_t days=seconds/86400;
    int64_t rest=seconds%86400;
    if(rest<0){rest+=86400;days--;}
    int64_t year;
    unsigned month,day;
    civil_from_days(days,&year,&month,&day);
    tm->sec=(int32_t)(rest%60);
    tm->min=(int32_t)((rest/60)%60);
    tm->hour=(int32_t)(rest/3600);
    tm->mday=(int32_t)day;
    tm->mon=(int32_t)month-1;
    tm->year=(int32_t)(year-1900);
    /* 1970-01-01 was a Thursday (4). */
    int64_t wday=(days+4)%7;
    if(wday<0)wday+=7;
    tm->wday=(int32_t)wday;
    tm->yday=(int32_t)(days-days_from_civil(year,1,1));
    tm->isdst=0;
}
static int64_t seconds_from_tm(const GuestTm *tm){
    int64_t days=days_from_civil((int64_t)tm->year+1900,(unsigned)(tm->mon+1),
                                 (unsigned)tm->mday);
    return days*86400+(int64_t)tm->hour*3600+(int64_t)tm->min*60+tm->sec;
}
static int read_tm(GuestCrt *crt,uint32_t address,GuestTm *tm){
    if(!address)return gcrt_stop(crt,"time: NULL struct tm");
    uint8_t bytes[36];
    int rc=gm_read(gcrt_memory(crt),address,bytes,36);
    if(rc)return rc;
    int32_t *fields=(int32_t *)tm;
    for(uint32_t i=0;i<9;i++){
        uint32_t value=(uint32_t)bytes[i*4]|((uint32_t)bytes[i*4+1]<<8)|
                       ((uint32_t)bytes[i*4+2]<<16)|((uint32_t)bytes[i*4+3]<<24);
        fields[i]=(int32_t)value;
    }
    return GM_OK;
}
static int write_tm(GuestCrt *crt,uint32_t address,const GuestTm *tm){
    if(!address)return gcrt_stop(crt,"time: NULL struct tm");
    uint8_t bytes[36];
    const int32_t *fields=(const int32_t *)tm;
    for(uint32_t i=0;i<9;i++){
        uint32_t value=(uint32_t)fields[i];
        bytes[i*4]=(uint8_t)value;bytes[i*4+1]=(uint8_t)(value>>8);
        bytes[i*4+2]=(uint8_t)(value>>16);bytes[i*4+3]=(uint8_t)(value>>24);
    }
    return gm_write(gcrt_memory(crt),address,bytes,36);
}
static int crt_time64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t out;
    int rc=gcrt_arg32(crt,sp,0,&out);
    if(rc)return rc;
    int64_t seconds=gcrt_now_seconds(crt);
    if(out){
        uint8_t bytes[8];
        uint64_t value=(uint64_t)seconds;
        for(uint32_t i=0;i<8;i++)bytes[i]=(uint8_t)(value>>(8*i));
        rc=gm_write(gcrt_memory(crt),out,bytes,8);
        if(rc)return rc;
    }
    ret->eax=(uint32_t)(uint64_t)seconds;
    ret->edx=(uint32_t)((uint64_t)seconds>>32);
    ret->edx_valid=1;
    return GM_OK;
}
static int gmtime_common(GuestCrt *crt,GcrtReturn *ret,int64_t seconds){
    GuestTm tm;
    tm_from_seconds(seconds,&tm);
    uint32_t buffer=0;
    int rc=gcrt_scratch(crt,GCRT_SLOT_TM,36,&buffer);
    if(rc)return rc;
    rc=write_tm(crt,buffer,&tm);
    if(rc)return rc;
    ret->eax=buffer;return GM_OK;
}
static int crt_gmtime64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t low,high;
    int rc=gcrt_arg32(crt,sp,0,&low);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&high);
    if(rc)return rc;
    return gmtime_common(crt,ret,(int64_t)(((uint64_t)high<<32)|low));
}
static int crt_localtime64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t low,high;
    int rc=gcrt_arg32(crt,sp,0,&low);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&high);
    if(rc)return rc;
    int64_t seconds=(int64_t)(((uint64_t)high<<32)|low);
    seconds-=(int64_t)gcrt_timezone_bias(crt)*60;
    return gmtime_common(crt,ret,seconds);
}
static int crt_localtime64_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t out,low,high;
    int rc=gcrt_arg32(crt,sp,0,&out);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&low);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&high);
    if(rc)return rc;
    if(!out){
        gcrt_set_errno(crt,GCRT_EINVAL);
        ret->eax=GCRT_EINVAL;return GM_OK;
    }
    int64_t seconds=(int64_t)(((uint64_t)high<<32)|low);
    seconds-=(int64_t)gcrt_timezone_bias(crt)*60;
    GuestTm tm;
    tm_from_seconds(seconds,&tm);
    rc=write_tm(crt,out,&tm);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}
static int crt_mktime64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    GuestTm tm;
    rc=read_tm(crt,address,&tm);
    if(rc)return rc;
    int64_t seconds=seconds_from_tm(&tm)+(int64_t)gcrt_timezone_bias(crt)*60;
    GuestTm normalized;
    tm_from_seconds(seconds-(int64_t)gcrt_timezone_bias(crt)*60,&normalized);
    normalized.isdst=tm.isdst;
    rc=write_tm(crt,address,&normalized);
    if(rc)return rc;
    ret->eax=(uint32_t)(uint64_t)seconds;
    ret->edx=(uint32_t)((uint64_t)seconds>>32);
    ret->edx_valid=1;
    return GM_OK;
}
static int crt_timezone(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;(void)ret;
    return gcrt_stop(crt,"__timezone: data import, not a function bridge");
}

/* ------------------------------------------------------------------ */
/* utility                                                             */
/* ------------------------------------------------------------------ */

static int crt_rand(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)sp;
    uint32_t *state=gcrt_rand_state(crt);
    if(!state)return GM_RANGE;
    *state=(*state)*214013u+2531011u;
    ret->eax=(*state>>16)&0x7fffu;
    return GM_OK;
}
static int crt_srand(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    (void)ret;
    uint32_t seed;
    int rc=gcrt_arg32(crt,sp,0,&seed);
    if(rc)return rc;
    uint32_t *state=gcrt_rand_state(crt);
    if(!state)return GM_RANGE;
    *state=seed;
    return GM_OK;
}
static int compare(GuestCrt *crt,uint32_t compar,uint32_t left,uint32_t right,
                   int32_t *result){
    uint32_t args[2]={left,right},eax=0;
    int rc=guest_call(crt,compar,args,2,&eax);
    if(rc)return rc;
    *result=(int32_t)eax;
    return GM_OK;
}
static int crt_bsearch(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t key,base,count,width,compar;
    int rc=gcrt_arg32(crt,sp,0,&key);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&base);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&width);
    if(!rc)rc=gcrt_arg32(crt,sp,4,&compar);
    if(rc)return rc;
    if(!count || !width){ret->eax=0;return GM_OK;}
    uint32_t low=0,high=count;
    while(low<high){
        uint32_t middle=low+(high-low)/2;
        int32_t order=0;
        rc=compare(crt,compar,key,base+middle*width,&order);
        if(rc)return rc;
        if(order<0)high=middle;
        else if(order>0)low=middle+1;
        else{ret->eax=base+middle*width;return GM_OK;}
    }
    ret->eax=0;return GM_OK;
}
static int crt_qsort(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t base,count,width,compar;
    int rc=gcrt_arg32(crt,sp,0,&base);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&width);
    if(!rc)rc=gcrt_arg32(crt,sp,3,&compar);
    if(rc)return rc;
    if(count<2 || !width){ret->eax=0;return GM_OK;}
    uint64_t total=(uint64_t)count*width;
    if(total>UINT64_C(0x4000000))
        return gcrt_stop(crt,"qsort: more than 64 MiB of elements");
    uint32_t temp=0;
    rc=gcrt_heap_alloc(crt,0,(uint32_t)total,&temp);
    if(rc)return rc;
    if(!temp)return gcrt_stop(crt,"qsort: guest heap exhausted");
    uint8_t *left=malloc(width),*right=malloc(width);
    if(!left || !right){free(left);free(right);gcrt_heap_free(crt,temp);return GM_ALLOCATION;}
    /* Bottom-up merge sort: deterministic, O(n log n), comparator order only. */
    for(uint32_t run=1;run<count;run*=2){
        for(uint32_t start=0;start<count;start+=2*run){
            uint32_t middle=start+run<count?start+run:count;
            uint32_t end=middle+run<count?middle+run:count;
            uint32_t i=start,j=middle,k=start;
            while(i<middle && j<end){
                int32_t order=0;
                rc=compare(crt,compar,base+i*width,base+j*width,&order);
                if(rc)goto done;
                uint32_t source=order<=0 ? i++ : j++;
                rc=gm_read(gcrt_memory(crt),base+source*width,left,width);
                if(rc)goto done;
                rc=gm_write(gcrt_memory(crt),temp+k*width,left,width);
                if(rc)goto done;
                k++;
            }
            while(i<middle){
                rc=gm_read(gcrt_memory(crt),base+i*width,left,width);
                if(rc)goto done;
                rc=gm_write(gcrt_memory(crt),temp+k*width,left,width);
                if(rc)goto done;
                i++;k++;
            }
            while(j<end){
                rc=gm_read(gcrt_memory(crt),base+j*width,right,width);
                if(rc)goto done;
                rc=gm_write(gcrt_memory(crt),temp+k*width,right,width);
                if(rc)goto done;
                j++;k++;
            }
        }
        /* Copy the merged runs back for the next pass. */
        for(uint32_t i=0;i<count && !rc;i++){
            rc=gm_read(gcrt_memory(crt),temp+i*width,left,width);
            if(!rc)rc=gm_write(gcrt_memory(crt),base+i*width,left,width);
        }
        if(rc)goto done;
        if(run>count/2)break;
    }
done:
    free(left);free(right);
    int free_rc=gcrt_heap_free(crt,temp);
    if(rc)return rc;
    if(free_rc)return free_rc;
    ret->eax=0;return GM_OK;
}

/* ------------------------------------------------------------------ */
/* math (double results on the x87 stack)                              */
/* ------------------------------------------------------------------ */

static int read_double_arg(GuestCrt *crt,uint32_t sp,uint32_t index,double *value){
    uint32_t low,high;
    int rc=gcrt_arg32(crt,sp,index,&low);
    if(!rc)rc=gcrt_arg32(crt,sp,index+1,&high);
    if(rc)return rc;
    uint64_t bits=((uint64_t)high<<32)|low;
    memcpy(value,&bits,8);
    return GM_OK;
}
static int return_double(GcrtReturn *ret,double value){
    uint64_t bits;
    memcpy(&bits,&value,8);
    ret->st0_bits=bits;ret->st0_valid=1;
    return GM_OK;
}
static int write_double(GuestCrt *crt,uint32_t address,double value){
    if(!address)return gcrt_stop(crt,"math: NULL output pointer");
    uint64_t bits;
    memcpy(&bits,&value,8);
    uint8_t bytes[8];
    for(uint32_t i=0;i<8;i++)bytes[i]=(uint8_t)(bits>>(8*i));
    return gm_write(gcrt_memory(crt),address,bytes,8);
}
static int crt_frexp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    double value;
    uint32_t exponent;
    int rc=read_double_arg(crt,sp,0,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&exponent);
    if(rc)return rc;
    int exp=0;
    double fraction=frexp(value,&exp);
    if(exponent){
        uint8_t bytes[4]={(uint8_t)exp,(uint8_t)((uint32_t)exp>>8),
                          (uint8_t)((uint32_t)exp>>16),(uint8_t)((uint32_t)exp>>24)};
        rc=gm_write(gcrt_memory(crt),exponent,bytes,4);
        if(rc)return rc;
    }
    return return_double(ret,fraction);
}
static int crt_ldexp(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    double value;
    uint32_t exponent;
    int rc=read_double_arg(crt,sp,0,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&exponent);
    if(rc)return rc;
    return return_double(ret,ldexp(value,(int)exponent));
}
/* The remaining libm entry points the UCRT exports. They are not static
 * imports of the recovered modules, but a guest that resolves them through
 * GetProcAddress must not fall into an unimplemented trap. Same convention:
 * arguments on the stack, result in GcrtReturn.st0_bits. */
#define MATH1(name,expr) \
    static int crt_math_##name(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){ \
        double x; \
        int rc=read_double_arg(crt,sp,0,&x); \
        if(rc)return rc; \
        return return_double(ret,(expr)); \
    }
#define MATH2(name,expr) \
    static int crt_math_##name(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){ \
        double x,y; \
        int rc=read_double_arg(crt,sp,0,&x); \
        if(!rc)rc=read_double_arg(crt,sp,2,&y); \
        if(rc)return rc; \
        return return_double(ret,(expr)); \
    }
MATH1(sin,sin(x))
MATH1(cos,cos(x))
MATH1(tan,tan(x))
MATH1(asin,asin(x))
MATH1(acos,acos(x))
MATH1(atan,atan(x))
MATH1(sinh,sinh(x))
MATH1(cosh,cosh(x))
MATH1(tanh,tanh(x))
MATH1(sqrt,sqrt(x))
MATH1(exp,exp(x))
MATH1(log,log(x))
MATH1(log10,log10(x))
MATH1(floor,floor(x))
MATH1(ceil,ceil(x))
MATH1(fabs,fabs(x))
MATH2(pow,pow(x,y))
MATH2(atan2,atan2(x,y))
MATH2(fmod,fmod(x,y))

static int crt_modf(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    double value;
    uint32_t integer_part;
    int rc=read_double_arg(crt,sp,0,&value);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&integer_part);
    if(rc)return rc;
    double integral=0.0;
    double fraction=modf(value,&integral);
    rc=write_double(crt,integer_part,integral);
    if(rc)return rc;
    return return_double(ret,fraction);
}

/* ------------------------------------------------------------------ */
/* filesystem                                                          */
/* ------------------------------------------------------------------ */

/* PE32 struct _stat64 (Win32 packing): 56 bytes.
 *   0 dev(4) 4 ino(2) 6 mode(2) 8 nlink(2) 10 uid(2) 12 gid(2) 16 rdev(4)
 *   24 size(8) 32 atime(8) 40 mtime(8) 48 ctime(8) */
#define GCRT_STAT64_SIZE 56u
static int crt_stat64(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t path,out;
    int rc=gcrt_arg32(crt,sp,0,&path);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&out);
    if(rc)return rc;
    GuestFileApi *files=gcrt_files(crt);
    if(!files)return gcrt_stop(crt,"_stat64: no file API attached");
    if(!out){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return (ret->eax=0xffffffffu),GM_OK;
    }
    uint32_t data=0;
    rc=gcrt_scratch(crt,GCRT_SLOT_IOBUF,1024,&data);
    if(rc)return rc;
    uint32_t result=0;
    rc=gfa_get_file_attributes_ex_a(gcrt_memory(crt),gcrt_teb(crt),files,path,0,data,
                                     &result);
    if(rc)return rc;
    if(!result){
        uint32_t error=0;
        uint8_t bytes[4];
        if(!gm_read(gcrt_memory(crt),gcrt_teb(crt)+0x34,bytes,4))
            error=(uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|
                  ((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);
        gcrt_set_errno(crt,error?error:GCRT_ENOENT);
        return (ret->eax=0xffffffffu),GM_OK;
    }
    uint8_t attributes[GFA_FILE_ATTRIBUTE_DATA_SIZE];
    rc=gm_read(gcrt_memory(crt),data,attributes,GFA_FILE_ATTRIBUTE_DATA_SIZE);
    if(rc)return rc;
    uint32_t file_attributes=(uint32_t)attributes[0]|((uint32_t)attributes[1]<<8)|
                             ((uint32_t)attributes[2]<<16)|((uint32_t)attributes[3]<<24);
    uint64_t size=0,write_time=0;
    /* WIN32_FILE_ATTRIBUTE_DATA: nFileSizeHigh at +28, nFileSizeLow at +32. */
    size=((uint64_t)((uint32_t)attributes[28]|((uint32_t)attributes[29]<<8)|
                     ((uint32_t)attributes[30]<<16)|((uint32_t)attributes[31]<<24))<<32)|
         (uint64_t)((uint32_t)attributes[32]|((uint32_t)attributes[33]<<8)|
                    ((uint32_t)attributes[34]<<16)|((uint32_t)attributes[35]<<24));
    for(uint32_t i=0;i<8;i++)write_time|=(uint64_t)attributes[20+i]<<(8*i);
    int directory=(file_attributes&0x10)!=0;
    uint8_t block[GCRT_STAT64_SIZE];
    memset(block,0,sizeof(block));
    uint32_t dev=0;
    uint16_t mode=(uint16_t)(directory ? 0040755u : 0100644u);
    block[0]=(uint8_t)dev;block[1]=(uint8_t)(dev>>8);block[2]=(uint8_t)(dev>>16);
    block[3]=(uint8_t)(dev>>24);
    block[6]=(uint8_t)mode;block[7]=(uint8_t)(mode>>8);
    block[8]=1;block[9]=0;             /* st_nlink */
    for(uint32_t i=0;i<8;i++)block[24+i]=(uint8_t)(size>>(8*i));
    int64_t seconds=(int64_t)(write_time/10000000u)-INT64_C(11644473600);
    uint64_t stamp=(uint64_t)seconds;
    for(uint32_t i=0;i<8;i++){
        block[32+i]=(uint8_t)(stamp>>(8*i));
        block[40+i]=(uint8_t)(stamp>>(8*i));
        block[48+i]=(uint8_t)(stamp>>(8*i));
    }
    rc=gm_write(gcrt_memory(crt),out,block,GCRT_STAT64_SIZE);
    if(rc)return rc;
    ret->eax=0;return GM_OK;
}
static int split_path(const uint16_t *path,uint32_t units,
                      uint32_t *drive_end,uint32_t *dir_start,uint32_t *dir_end,
                      uint32_t *name_start,uint32_t *name_end,uint32_t *ext_start){
    uint32_t i=0;
    *drive_end=0;*dir_start=0;*dir_end=0;*name_start=0;*name_end=units;*ext_start=units;
    if(units>=2 && path[1]==':'){
        *drive_end=2;
        *dir_start=2;
        i=2;
    }
    for(uint32_t k=i;k<units;k++)if(path[k]=='/'||path[k]=='\\')*dir_end=k+1;
    uint32_t name=*dir_end;
    for(uint32_t k=name;k<units;k++)if(path[k]=='.')*ext_start=k;
    if(*ext_start<name)*ext_start=units;
    *name_start=name;
    *name_end=*ext_start;
    return GM_OK;
}
static int copy_units(GuestCrt *crt,const uint16_t *units,uint32_t count,uint32_t out,
                      uint32_t capacity,uint32_t *error){
    if(!out){
        if(capacity){*error=GCRT_EINVAL;return GM_OK;}
        return GM_OK;
    }
    if(count+1>capacity){*error=GCRT_ERANGE;return GM_OK;}
    uint8_t *bytes=malloc((count+1)*2);
    if(!bytes)return GM_ALLOCATION;
    for(uint32_t i=0;i<count;i++){
        bytes[i*2]=(uint8_t)units[i];bytes[i*2+1]=(uint8_t)(units[i]>>8);
    }
    bytes[count*2]=0;bytes[count*2+1]=0;
    int rc=gm_write(gcrt_memory(crt),out,bytes,(count+1)*2);
    free(bytes);return rc;
}
static int crt_wsplitpath_s(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t args[8];
    for(uint32_t i=0;i<8;i++){
        int rc=gcrt_arg32(crt,sp,i,&args[i]);
        if(rc)return rc;
    }
    uint32_t path=args[0],drive=args[1],drive_count=args[2],dir=args[3],
             dir_count=args[4],name=args[5],name_count=args[6],ext=args[7];
    uint32_t ext_count;
    /* The ninth argument (extCount) follows the eight listed ones. */
    {
        int rc=gcrt_arg32(crt,sp,8,&ext_count);
        if(rc)return rc;
    }
    if(!path){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return (ret->eax=GCRT_EINVAL),GM_OK;
    }
    uint32_t units=0;
    int rc=gcrt_strlen_wide(crt,path,1024,&units);
    if(rc)return rc;
    uint16_t buffer[1024];
    uint8_t *raw=malloc(units*2+2);
    if(!raw)return GM_ALLOCATION;
    rc=gm_read(gcrt_memory(crt),path,raw,units*2);
    if(rc){free(raw);return rc;}
    for(uint32_t i=0;i<units;i++)buffer[i]=(uint16_t)(raw[i*2]|(raw[i*2+1]<<8));
    free(raw);
    uint32_t drive_end,dir_start,dir_end,name_start,name_end,ext_start;
    split_path(buffer,units,&drive_end,&dir_start,&dir_end,&name_start,&name_end,
               &ext_start);
    uint32_t error=0;
    rc=copy_units(crt,buffer,drive_end,drive,drive_count,&error);
    if(!rc && !error)rc=copy_units(crt,buffer+dir_start,dir_end-dir_start,dir,dir_count,
                                   &error);
    if(!rc && !error)rc=copy_units(crt,buffer+name_start,name_end-name_start,name,
                                   name_count,&error);
    if(!rc && !error)rc=copy_units(crt,buffer+ext_start,units-ext_start,ext,ext_count,
                                   &error);
    if(rc)return rc;
    if(error){
        gcrt_set_errno(crt,error);
        ret->eax=error;return GM_OK;
    }
    ret->eax=0;return GM_OK;
}

#define RUNTIME_DLL "api-ms-win-crt-runtime-l1-1-0.dll"
#define TIME_DLL "api-ms-win-crt-time-l1-1-0.dll"
#define UTILITY_DLL "api-ms-win-crt-utility-l1-1-0.dll"
#define MATH_DLL "api-ms-win-crt-math-l1-1-0.dll"
#define FILESYSTEM_DLL "api-ms-win-crt-filesystem-l1-1-0.dll"
const GcrtExport crt_misc_exports[]={
    {RUNTIME_DLL,"_errno",0,crt_errno_fn},
    {RUNTIME_DLL,"_invalid_parameter_noinfo",0,crt_invalid_parameter_noinfo},
    {RUNTIME_DLL,"_invalid_parameter_noinfo_noreturn",0,crt_invalid_parameter_noreturn},
    {RUNTIME_DLL,"_resetstkoflw",0,crt_resetstkoflw},
    {RUNTIME_DLL,"exit",0,crt_exit},
    {RUNTIME_DLL,"strerror",0,crt_strerror},
    {RUNTIME_DLL,"_initterm",0,crt_initterm_plain},
    {RUNTIME_DLL,"_initterm_e",0,crt_initterm_e},
    {TIME_DLL,"__timezone",0,crt_timezone},
    {TIME_DLL,"_gmtime64",0,crt_gmtime64},
    {TIME_DLL,"_localtime64",0,crt_localtime64},
    {TIME_DLL,"_localtime64_s",0,crt_localtime64_s},
    {TIME_DLL,"_mktime64",0,crt_mktime64},
    {TIME_DLL,"_time64",0,crt_time64},
    {UTILITY_DLL,"bsearch",0,crt_bsearch},
    {UTILITY_DLL,"qsort",0,crt_qsort},
    {UTILITY_DLL,"rand",0,crt_rand},
    {UTILITY_DLL,"srand",0,crt_srand},
    {MATH_DLL,"frexp",0,crt_frexp},
    {MATH_DLL,"ldexp",0,crt_ldexp},
    {MATH_DLL,"modf",0,crt_modf},
    {MATH_DLL,"sin",0,crt_math_sin},
    {MATH_DLL,"cos",0,crt_math_cos},
    {MATH_DLL,"tan",0,crt_math_tan},
    {MATH_DLL,"asin",0,crt_math_asin},
    {MATH_DLL,"acos",0,crt_math_acos},
    {MATH_DLL,"atan",0,crt_math_atan},
    {MATH_DLL,"sinh",0,crt_math_sinh},
    {MATH_DLL,"cosh",0,crt_math_cosh},
    {MATH_DLL,"tanh",0,crt_math_tanh},
    {MATH_DLL,"sqrt",0,crt_math_sqrt},
    {MATH_DLL,"exp",0,crt_math_exp},
    {MATH_DLL,"log",0,crt_math_log},
    {MATH_DLL,"log10",0,crt_math_log10},
    {MATH_DLL,"floor",0,crt_math_floor},
    {MATH_DLL,"ceil",0,crt_math_ceil},
    {MATH_DLL,"fabs",0,crt_math_fabs},
    {MATH_DLL,"pow",0,crt_math_pow},
    {MATH_DLL,"atan2",0,crt_math_atan2},
    {MATH_DLL,"fmod",0,crt_math_fmod},
    {FILESYSTEM_DLL,"_stat64",0,crt_stat64},
    {FILESYSTEM_DLL,"_wsplitpath_s",0,crt_wsplitpath_s}
};
const uint32_t crt_misc_export_count=sizeof(crt_misc_exports)/sizeof(crt_misc_exports[0]);
