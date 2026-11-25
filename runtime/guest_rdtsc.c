#include <stdint.h>
#include <time.h>
/* Guest time-stamp counter: monotonic nanoseconds scaled to a nominal 3 GHz. */
uint64_t guest_rdtsc(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return ((uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec) * 3ull; }
