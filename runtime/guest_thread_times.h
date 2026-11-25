#ifndef GUEST_THREAD_TIMES_H
#define GUEST_THREAD_TIMES_H
#include "guest_memory.h"
typedef struct {uint64_t creation,exit,kernel,user;} GuestThreadTimes;
typedef struct {GuestThreadTimes origin;int64_t owner;} GuestThreadClock;
/* One guest CPU bound to one dedicated host thread. Values in 100ns units. */
int gtt_start(GuestThreadClock *);
int gtt_sample(const GuestThreadClock *,GuestThreadTimes *);
int gtt_write(GuestMemory *,uint32_t handle,const uint32_t outputs[4],const GuestThreadTimes *);
#endif
