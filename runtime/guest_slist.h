#ifndef BURNOUT_GUEST_SLIST_H
#define BURNOUT_GUEST_SLIST_H
#include "guest_memory.h"
/* PE32 SLIST_HEADER is eight bytes. Only initialization is implemented here. */
int gsl_initialize(GuestMemory *,uint32_t header);
#endif
