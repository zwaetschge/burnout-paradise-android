#define _GNU_SOURCE
#include "guest_thread_times.h"
#include "guest_system_time.h"
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
static int cpu_times(uint64_t *kernel,uint64_t *user){
    struct rusage usage;if(getrusage(RUSAGE_THREAD,&usage))return GM_RANGE;
    struct timeval values[2]={usage.ru_stime,usage.ru_utime};uint64_t ticks[2];
    for(unsigned i=0;i<2;i++){
        if(values[i].tv_sec<0 || values[i].tv_usec<0 || values[i].tv_usec>=1000000)return GM_RANGE;
        uint64_t sec=(uint64_t)values[i].tv_sec,fraction=(uint64_t)values[i].tv_usec*10u;
        if(sec>(UINT64_MAX-fraction)/10000000u)return GM_RANGE;
        ticks[i]=sec*10000000u+fraction;
    }
    *kernel=ticks[0];*user=ticks[1];return GM_OK;
}
int gtt_start(GuestThreadClock *clock){
    if(!clock)return GM_RANGE;
    GuestThreadClock value={0};int rc=gst_now(&value.origin.creation);if(rc)return rc;
    rc=cpu_times(&value.origin.kernel,&value.origin.user);if(rc)return rc;
    value.owner=(int64_t)syscall(SYS_gettid);if(value.owner<=0)return GM_RANGE;
    *clock=value;return GM_OK;
}
int gtt_sample(const GuestThreadClock *clock,GuestThreadTimes *times){
    if(!clock || !times || !clock->origin.creation || clock->owner!=(int64_t)syscall(SYS_gettid))return GM_UNSUPPORTED;
    GuestThreadTimes value={clock->origin.creation,0,0,0};
    int rc=cpu_times(&value.kernel,&value.user);if(rc)return rc;
    if(value.kernel<clock->origin.kernel || value.user<clock->origin.user)return GM_RANGE;
    value.kernel-=clock->origin.kernel;value.user-=clock->origin.user;
    /* Running thread: ExitTime is undefined by Win32; this runtime emits 0. */
    *times=value;return GM_OK;
}
int gtt_write(GuestMemory *memory,uint32_t handle,const uint32_t outputs[4],const GuestThreadTimes *times){
    if(handle!=UINT32_C(0xfffffffe) || !outputs || !times || !times->creation || times->exit)return GM_UNSUPPORTED;
    for(unsigned i=0;i<4;i++)if(outputs[i]){int rc=gm_check(memory,outputs[i],8,GM_WRITE);if(rc)return rc;}
    uint64_t values[4]={times->creation,times->exit,times->kernel,times->user};
    for(unsigned i=0;i<4;i++)if(outputs[i]){
        uint8_t bytes[8];for(unsigned j=0;j<8;j++)bytes[j]=(uint8_t)(values[i]>>(8*j));
        int rc=gm_write(memory,outputs[i],bytes,8);if(rc)return rc;
    }
    return GM_OK;
}
