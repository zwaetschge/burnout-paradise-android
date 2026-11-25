#include "guest_time_convert.h"
#include "guest_system_time.h"

/* Days since 1970-01-01 for a proleptic Gregorian civil date (Hinnant). */
static int64_t days_from_civil(int64_t y,unsigned m,unsigned d){
    y-=(m<=2);
    const int64_t era=(y>=0?y:y-399)/400;
    const unsigned yoe=(unsigned)(y-era*400);
    const unsigned doy=(153u*(m+(m>2?-3u:9u))+2u)/5u+d-1u;
    const unsigned doe=yoe*365u+yoe/4u-yoe/100u+doy;
    return era*146097+(int64_t)doe-719468;
}
static void civil_from_days(int64_t z,int64_t *y,unsigned *m,unsigned *d){
    z+=719468;
    const int64_t era=(z>=0?z:z-146096)/146097;
    const unsigned doe=(unsigned)(z-era*146097);
    const unsigned yoe=(doe-doe/1460u+doe/36524u-doe/146096u)/365u;
    const int64_t year=(int64_t)yoe+era*400;
    const unsigned doy=doe-(365u*yoe+yoe/4u-yoe/100u);
    const unsigned mp=(5u*doy+2u)/153u;
    *d=doy-(153u*mp+2u)/5u+1u;
    *m=mp<10u?mp+3u:mp-9u;
    *y=year+(*m<=2u?1:0);
}
static int is_leap(int64_t y){
    return (y%4==0 && y%100!=0) || y%400==0;
}
static unsigned month_days(int64_t y,unsigned m){
    static const unsigned days[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    return (m==2u && is_leap(y))?29u:days[m-1u];
}
/* 1970-01-01 was a Thursday; SYSTEMTIME wDayOfWeek is 0=Sunday. */
static uint32_t day_of_week(int64_t days){
    int64_t value=(days+4)%7;
    if(value<0)value+=7;
    return (uint32_t)value;
}
int gtc_civil_to_filetime(uint32_t year,uint32_t month,uint32_t day,uint32_t hour,
                          uint32_t minute,uint32_t second,uint32_t milliseconds,
                          uint64_t *filetime){
    if(!filetime)return GM_UNSUPPORTED;
    if(year<GTC_MIN_YEAR || year>GTC_MAX_YEAR)return GM_UNSUPPORTED;
    if(month<1u || month>12u)return GM_UNSUPPORTED;
    if(day<1u || day>month_days(year,month))return GM_UNSUPPORTED;
    if(hour>23u || minute>59u || second>59u || milliseconds>999u)return GM_UNSUPPORTED;
    int64_t days=days_from_civil(year,month,day);
    int64_t seconds=days*86400+hour*3600+minute*60+second;
    int64_t offset=seconds*10000000+(int64_t)milliseconds*10000;
    /* 1601-01-01 itself is a negative offset from the 1970 FILETIME epoch. */
    if(offset<-(int64_t)GTC_EPOCH_TICKS)return GM_UNSUPPORTED;
    *filetime=GTC_EPOCH_TICKS+(uint64_t)offset;
    return GM_OK;
}
int gtc_filetime_to_civil(uint64_t filetime,uint32_t fields[8]){
    if(!fields)return GM_UNSUPPORTED;
    /* Signed 100 ns offset from 1970-01-01; FILETIMEs before 1970 (down to
     * 1601-01-01 = 0) are valid and must not be rejected. */
    int64_t ticks;
    if(filetime>=GTC_EPOCH_TICKS)ticks=(int64_t)(filetime-GTC_EPOCH_TICKS);
    else{
        uint64_t delta=GTC_EPOCH_TICKS-filetime;
        if(delta>(uint64_t)INT64_MAX)return GM_UNSUPPORTED;
        ticks=-(int64_t)delta;
    }
    int64_t seconds=ticks/10000000;
    int64_t sub=ticks%10000000;
    if(sub<0){sub+=10000000;seconds--;}
    int64_t days=seconds/86400;
    int64_t remainder=seconds%86400;
    if(remainder<0){remainder+=86400;days--;}
    uint32_t milliseconds=(uint32_t)(sub/10000);
    int64_t year;unsigned month,day;
    civil_from_days(days,&year,&month,&day);
    if(year<(int64_t)GTC_MIN_YEAR || year>(int64_t)GTC_MAX_YEAR)return GM_UNSUPPORTED;
    fields[0]=(uint32_t)year;fields[1]=month;fields[2]=day_of_week(days);fields[3]=day;
    fields[4]=(uint32_t)(remainder/3600);fields[5]=(uint32_t)((remainder%3600)/60);
    fields[6]=(uint32_t)(remainder%60);fields[7]=milliseconds;
    return GM_OK;
}
static void put16(uint8_t *b,uint32_t v){
    b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);
}
static uint32_t get16(const uint8_t *b){return (uint32_t)b[0]|((uint32_t)b[1]<<8);}
static void put32(uint8_t *b,uint32_t v){
    b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);b[2]=(uint8_t)(v>>16);b[3]=(uint8_t)(v>>24);
}
static void put64(uint8_t *b,uint64_t v){
    for(unsigned i=0;i<8;i++)b[i]=(uint8_t)(v>>(8*i));
}
static uint64_t get64(const uint8_t *b){
    uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)b[i]<<(8*i);return v;
}
static int last_error(GuestMemory *memory,uint32_t teb,uint32_t value){
    uint8_t bytes[4];put32(bytes,value);return gm_write(memory,teb+0x34,bytes,4);
}
static int write_system_time(GuestMemory *memory,uint32_t output,const uint32_t fields[8]){
    uint8_t bytes[GTC_SYSTEMTIME_SIZE];
    for(unsigned i=0;i<8;i++)put16(bytes+i*2,fields[i]);
    return gm_write(memory,output,bytes,GTC_SYSTEMTIME_SIZE);
}
static int read_system_time(GuestMemory *memory,uint32_t input,uint32_t fields[8]){
    uint8_t bytes[GTC_SYSTEMTIME_SIZE];
    int rc=gm_read(memory,input,bytes,GTC_SYSTEMTIME_SIZE);if(rc)return rc;
    for(unsigned i=0;i<8;i++)fields[i]=get16(bytes+i*2);
    return GM_OK;
}
int gtc_system_time_to_filetime(GuestMemory *memory,uint32_t teb,uint32_t system_time,
                                uint32_t filetime,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint32_t f[8];int rc=read_system_time(memory,system_time,f);if(rc)return rc;
    /* wDayOfWeek is ignored by Win32; the date fields decide. */
    uint64_t ticks;
    rc=gtc_civil_to_filetime(f[0],f[1],f[3],f[4],f[5],f[6],f[7],&ticks);
    if(rc==GM_UNSUPPORTED){
        rc=last_error(memory,teb,GTC_ERROR_INVALID_PARAMETER);if(rc)return rc;
        *result=0;return GM_OK;
    }
    if(rc)return rc;
    uint8_t bytes[8];put64(bytes,ticks);
    rc=gm_write(memory,filetime,bytes,8);if(rc)return rc;
    *result=1;return GM_OK;
}
int gtc_filetime_to_system_time(GuestMemory *memory,uint32_t teb,uint32_t filetime,
                                uint32_t system_time,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint8_t bytes[8];int rc=gm_read(memory,filetime,bytes,8);if(rc)return rc;
    uint32_t f[8];
    rc=gtc_filetime_to_civil(get64(bytes),f);
    if(rc==GM_UNSUPPORTED){
        rc=last_error(memory,teb,GTC_ERROR_INVALID_PARAMETER);if(rc)return rc;
        *result=0;return GM_OK;
    }
    if(rc)return rc;
    rc=write_system_time(memory,system_time,f);if(rc)return rc;
    *result=1;return GM_OK;
}
int gtc_get_system_time(GuestMemory *memory,uint32_t output,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint64_t ticks;int rc=gst_now(&ticks);if(rc)return rc;
    uint32_t f[8];rc=gtc_filetime_to_civil(ticks,f);if(rc)return rc;
    rc=write_system_time(memory,output,f);if(rc)return rc;
    *result=1;return GM_OK;
}
int gtc_get_local_time(GuestMemory *memory,uint32_t output,int64_t bias_100ns,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint64_t ticks;int rc=gst_now(&ticks);if(rc)return rc;
    if(bias_100ns>=0){
        if((uint64_t)bias_100ns>UINT64_MAX-ticks)return GM_UNSUPPORTED;
        ticks+=(uint64_t)bias_100ns;
    }else{
        uint64_t magnitude=(uint64_t)(-(bias_100ns+1))+1u;
        if(magnitude>ticks)return GM_UNSUPPORTED;
        ticks-=magnitude;
    }
    uint32_t f[8];rc=gtc_filetime_to_civil(ticks,f);if(rc)return rc;
    rc=write_system_time(memory,output,f);if(rc)return rc;
    *result=1;return GM_OK;
}
int gtc_filetime_to_local_filetime(GuestMemory *memory,uint32_t teb,uint32_t input,
                                   uint32_t output,int64_t bias_100ns,uint32_t *result){
    (void)teb; /* FileTimeToLocalFileTime reports failure only through its BOOL. */
    if(!memory || !result)return GM_UNSUPPORTED;
    uint8_t bytes[8];int rc=gm_read(memory,input,bytes,8);if(rc)return rc;
    uint64_t ticks=get64(bytes);
    if(bias_100ns>=0){
        if((uint64_t)bias_100ns>UINT64_MAX-ticks)return GM_UNSUPPORTED;
        ticks+=(uint64_t)bias_100ns;
    }else{
        uint64_t magnitude=(uint64_t)(-(bias_100ns+1))+1u;
        if(magnitude>ticks)return GM_UNSUPPORTED;
        ticks-=magnitude;
    }
    put64(bytes,ticks);
    rc=gm_write(memory,output,bytes,8);if(rc)return rc;
    *result=1;return GM_OK;
}
/* Milliseconds since boot from the same monotonic provider as interrupt time. */
static int boot_milliseconds(uint64_t *milliseconds){
    uint64_t ticks;int rc=gst_interrupt_now(&ticks);if(rc)return rc;
    *milliseconds=ticks/10000u;return GM_OK;
}
int gtc_get_tick_count(uint32_t *result){
    if(!result)return GM_UNSUPPORTED;
    uint64_t milliseconds;int rc=boot_milliseconds(&milliseconds);if(rc)return rc;
    *result=(uint32_t)milliseconds;return GM_OK;
}
int gtc_get_tick_count64(GuestMemory *memory,uint32_t output,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint64_t milliseconds;int rc=boot_milliseconds(&milliseconds);if(rc)return rc;
    uint8_t bytes[8];put64(bytes,milliseconds);
    rc=gm_write(memory,output,bytes,8);if(rc)return rc;
    *result=(uint32_t)milliseconds;return GM_OK;
}
int gtc_time_get_time(uint32_t *result){
    return gtc_get_tick_count(result);
}
const GtcExport gtc_exports[]={
    {"kernel32.dll","GetSystemTime",4,(void*)gtc_get_system_time},
    {"kernel32.dll","GetLocalTime",4,(void*)gtc_get_local_time},
    {"kernel32.dll","SystemTimeToFileTime",8,(void*)gtc_system_time_to_filetime},
    {"kernel32.dll","FileTimeToSystemTime",8,(void*)gtc_filetime_to_system_time},
    {"kernel32.dll","FileTimeToLocalFileTime",8,(void*)gtc_filetime_to_local_filetime},
    {"kernel32.dll","GetTickCount",0,(void*)gtc_get_tick_count},
    {"kernel32.dll","GetTickCount64",0,(void*)gtc_get_tick_count64},
    {"winmm.dll","timeGetTime",0,(void*)gtc_time_get_time},
};
const uint32_t gtc_export_count=(uint32_t)(sizeof(gtc_exports)/sizeof(gtc_exports[0]));
