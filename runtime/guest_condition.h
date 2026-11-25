#ifndef BURNOUT_GUEST_CONDITION_H
#define BURNOUT_GUEST_CONDITION_H
#include "guest_memory.h"
/* PE32 initial empty condition variable. Waiting/waking are separate contracts. */
int gcv_initialize(GuestMemory *,uint32_t condition);
#endif
