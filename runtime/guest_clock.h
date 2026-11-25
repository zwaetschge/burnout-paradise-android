#ifndef BURNOUT_GUEST_CLOCK_H
#define BURNOUT_GUEST_CLOCK_H
#include "guest_memory.h"
#define GC_FREQUENCY UINT64_C(1000000000)
int gc_now_ns(uint64_t *nanoseconds);
int gc_frequency(GuestMemory *memory,uint32_t output);
int gc_counter_value(GuestMemory *memory,uint32_t output,uint64_t nanoseconds);
int gc_counter(GuestMemory *memory,uint32_t output);
#endif
