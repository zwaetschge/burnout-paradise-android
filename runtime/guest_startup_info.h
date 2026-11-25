#ifndef BURNOUT_GUEST_STARTUP_INFO_H
#define BURNOUT_GUEST_STARTUP_INFO_H
#include "guest_memory.h"
/* Project the port-owned PE32 process parameters into STARTUPINFOW (68 bytes).
 * Copies guest pointers/handle values only; creates no windows/console/handles.
 * Single guest thread, so no concurrent process-parameter mutation is allowed. */
int gsi_get(GuestMemory *,uint32_t peb,uint32_t output);
int gsi_std_handle(GuestMemory *,uint32_t peb,uint32_t selector,uint32_t *value,uint32_t *error);
#endif
