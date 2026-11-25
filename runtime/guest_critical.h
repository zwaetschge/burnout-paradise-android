#ifndef BURNOUT_GUEST_CRITICAL_H
#define BURNOUT_GUEST_CRITICAL_H
#include "guest_memory.h"
/* PE32 critical sections for the declared one-guest-CPU execution model.
 * Recursive ownership works; blocking on another guest thread is unsupported.
 * No host pointer is serialized into the 24-byte guest object. */
int gcs_initialize(GuestMemory *,uint32_t section,uint32_t spin,uint32_t flags,uint32_t processors);
int gcs_enter(GuestMemory *,uint32_t section,uint32_t thread,int try_only,uint32_t *entered);
int gcs_leave(GuestMemory *,uint32_t section,uint32_t thread);
int gcs_delete(GuestMemory *,uint32_t section);

/* ------------------------------------------------------------------ */
/* A14 -- multi-thread mode                                            */
/* ------------------------------------------------------------------ */
/* The default mode is the documented one-guest-CPU model above: blocking on
 * another guest thread is a named stop. When the runtime actually starts guest
 * threads (runtime/static_threads.c) it switches this module to multi-thread
 * mode, in which
 *
 *   - every critical-section guest address owns a host PTHREAD_MUTEX_RECURSIVE,
 *   - EnterCriticalSection blocks on it instead of stopping on contention,
 *   - recursion and ownership are still mirrored into the 24-byte guest object
 *     (RecursionCount/OwningThread) so guest code that inspects it sees the
 *     documented values,
 *   - TryEnterCriticalSection reports 0 instead of blocking,
 *   - DeleteCriticalSection releases the host object.
 *
 * The single-CPU code path is untouched when the mode is off, so the existing
 * differential tests keep their exact behaviour. */
void gcs_set_multithread(int enabled);
int gcs_multithread(void);
#endif
