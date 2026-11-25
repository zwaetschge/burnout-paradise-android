#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "guest_system_time.h"
#include "guest_memory.h"
#include <time.h>
int gst_now(uint64_t *value){
    if(!value)return GM_RANGE;
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME,&ts) || ts.tv_sec<0 || ts.tv_nsec<0 || ts.tv_nsec>=1000000000)return GM_RANGE;
    uint64_t sec=(uint64_t)ts.tv_sec,fraction=(uint64_t)ts.tv_nsec/100u;
    if(sec>(UINT64_MAX-GST_EPOCH_TICKS)/10000000u)return GM_RANGE;
    uint64_t base=GST_EPOCH_TICKS+sec*10000000u;
    if(fraction>UINT64_MAX-base)return GM_RANGE;
    *value=base+fraction;return 0;
}
int gst_next(GuestSystemTime *clock,uint64_t *value){
    if(!clock || !value || clock->reads==UINT64_MAX)return GM_RANGE;
    uint64_t ticks;
    if(clock->diagnostic){
        if(clock->step && clock->reads>(UINT64_MAX-clock->start)/clock->step)return GM_RANGE;
        if(clock->step && clock->reads>(UINT64_MAX-clock->interrupt_start)/clock->step)return GM_RANGE;
        ticks=clock->start+clock->reads*clock->step;
    }else{
        int rc=gst_now(&ticks);if(rc)return rc;
    }
    *value=ticks;clock->reads++;return 0;
}
int gst_interrupt_now(uint64_t *value){
    if(!value)return GM_RANGE;
#ifdef CLOCK_BOOTTIME
    struct timespec ts;
    if(clock_gettime(CLOCK_BOOTTIME,&ts) || ts.tv_sec<0 || ts.tv_nsec<0 || ts.tv_nsec>=1000000000)return GM_RANGE;
    uint64_t sec=(uint64_t)ts.tv_sec,fraction=(uint64_t)ts.tv_nsec/100u;
    if(sec>UINT64_MAX/10000000u)return GM_RANGE;
    uint64_t base=sec*10000000u;if(fraction>UINT64_MAX-base)return GM_RANGE;
    *value=base+fraction;return GM_OK;
#else
    return GM_UNSUPPORTED;
#endif
}
int gst_filetime_value(GuestMemory *memory,uint32_t output,uint64_t ticks){
    uint8_t bytes[8];for(unsigned i=0;i<8;i++)bytes[i]=(uint8_t)(ticks>>(8*i));
    return gm_write(memory,output,bytes,8);
}
int gst_interrupt_value(const GuestSystemTime *clock,uint64_t *value){
    if(!clock || !value || !clock->reads)return GM_RANGE;
    if(!clock->diagnostic)return gst_interrupt_now(value);
    uint64_t index=clock->reads-1;
    if(clock->step && index>(UINT64_MAX-clock->interrupt_start)/clock->step)return GM_RANGE;
    *value=clock->interrupt_start+index*clock->step;return GM_OK;
}
int gst_timezone_bias(uint64_t filetime,int64_t *bias){
    if(!bias || filetime<GST_EPOCH_TICKS)return GM_RANGE;
    uint64_t sec=(filetime-GST_EPOCH_TICKS)/10000000u;time_t stamp=(time_t)sec;
    if(stamp<0 || (uint64_t)stamp!=sec)return GM_RANGE;
    struct tm local;if(!localtime_r(&stamp,&local))return GM_RANGE;
    int64_t offset=local.tm_gmtoff;
    if(offset>INT64_MAX/10000000 || offset<INT64_MIN/10000000)return GM_RANGE;
    *bias=-(offset*10000000);return 0;
}
void gst_store(uint8_t *page,uint64_t ticks){
    for(unsigned i=0;i<8;i++)page[GST_OFFSET+i]=(uint8_t)(ticks>>(i*8));
    for(unsigned i=0;i<4;i++)page[GST_OFFSET+8+i]=(uint8_t)(ticks>>((i+4)*8));
}
void gst_store_bias(uint8_t *page,int64_t bias){
    uint64_t bits=(uint64_t)bias;
    for(unsigned i=0;i<8;i++)page[GST_ZONE_OFFSET+i]=(uint8_t)(bits>>(i*8));
    for(unsigned i=0;i<4;i++)page[GST_ZONE_OFFSET+8+i]=(uint8_t)(bits>>((i+4)*8));
}
void gst_store_interrupt(uint8_t *page,uint64_t ticks){
    for(unsigned i=0;i<8;i++)page[GST_INTERRUPT_OFFSET+i]=(uint8_t)(ticks>>(i*8));
    for(unsigned i=0;i<4;i++)page[GST_INTERRUPT_OFFSET+8+i]=(uint8_t)(ticks>>((i+4)*8));
}
