#ifndef BURNOUT_GUEST_SRW_H
#define BURNOUT_GUEST_SRW_H
#include "guest_memory.h"
/* Slim reader/writer locks for the declared one-guest-CPU runtime.
 *
 * The guest runtime owns exactly one guest CPU and never schedules a second
 * guest thread, so only the uncontended state is reachable. The 4-byte guest
 * object uses the documented Windows encoding: bit 0 = exclusive owner, upper
 * bits = shared owner count. Contention, recursion and cross-thread handoff
 * stop explicitly instead of synthesizing wait/wake behaviour. No host pointer
 * is ever serialized into the guest object.
 *
 * *result receives the Win32 return value of the Try* variants (0/1). The
 * VOID variants leave it untouched, matching the preserved EAX. A non-zero
 * function result is a model stop, not a Win32 status. */
enum { GSRW_ACQUIRE_EXCLUSIVE = 0, GSRW_TRY_ACQUIRE_EXCLUSIVE,
       GSRW_ACQUIRE_SHARED, GSRW_TRY_ACQUIRE_SHARED,
       GSRW_RELEASE_EXCLUSIVE, GSRW_RELEASE_SHARED };
typedef struct { uint32_t operations; } GuestSrw;
void gsrw_initialize(GuestSrw *);
int gsrw_call(GuestSrw *,GuestMemory *,uint32_t operation,uint32_t address,uint32_t *result);
uint32_t gsrw_operations(const GuestSrw *);

/* ------------------------------------------------------------------ */
/* A14 -- multi-thread mode                                            */
/* ------------------------------------------------------------------ */
/* Default: the one-guest-CPU model above (contention is a named stop). When the
 * runtime starts real guest threads it enables this mode, in which every SRW
 * guest address owns a host pthread_rwlock_t:
 *
 *   AcquireSRWLockExclusive  -> pthread_rwlock_wrlock  (blocks)
 *   AcquireSRWLockShared     -> pthread_rwlock_rdlock  (blocks)
 *   Release*                 -> pthread_rwlock_unlock
 *   Try*                     -> trywrlock/tryrdlock, 0 when busy
 *
 * The 4-byte guest object keeps the documented encoding (bit 0 = exclusive,
 * upper bits = shared count) as a mirror of the host lock, updated under a
 * module mutex so two guest threads cannot corrupt the count. The single-CPU
 * code path is untouched when the mode is off. */
void gsrw_set_multithread(int enabled);
int gsrw_multithread(void);
#endif
