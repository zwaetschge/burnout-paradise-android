#define _POSIX_C_SOURCE 200809L
#include "guest_clock.h"
#include <time.h>
int gc_now_ns(uint64_t *value) {
#ifdef CLOCK_BOOTTIME
    /* QPC includes sleep time. BOOTTIME keeps that property on Linux/Android.
     * https://learn.microsoft.com/windows/win32/sysinfo/acquiring-high-resolution-time-stamps */
    struct timespec ts;
    if(clock_gettime(CLOCK_BOOTTIME,&ts))return GM_RANGE;
    *value=(uint64_t)ts.tv_sec*GC_FREQUENCY+(uint64_t)ts.tv_nsec;return GM_OK;
#else
    (void)value;return GM_UNSUPPORTED;
#endif
}
int gc_counter_value(GuestMemory *m,uint32_t output,uint64_t ns) {
    uint8_t bytes[8];for(unsigned i=0;i<8;i++)bytes[i]=(uint8_t)(ns>>(8*i));
    return gm_write(m,output,bytes,8);
}
int gc_frequency(GuestMemory *m,uint32_t output) {return gc_counter_value(m,output,GC_FREQUENCY);}
int gc_counter(GuestMemory *m,uint32_t output) {
    uint64_t ns=0;int rc=gc_now_ns(&ns);return rc ? rc : gc_counter_value(m,output,ns);
}
