#ifndef BURNOUT_GUEST_HANDLES_H
#define BURNOUT_GUEST_HANDLES_H
#include <stdint.h>
/* Shared Win32 kernel-object handle table for all bridges (files, events,
 * threads, mutexes, sockets-as-handles, ...). Guest handles are opaque
 * multiples of 4 starting at 0x2000; 0 and 0xffffffff are never issued.
 * Each entry stores a kind tag and a host-side object pointer that is NEVER
 * written into guest memory. Close dispatches to the kind's destructor.
 * Thread-safe (internal mutex); refcounted for DuplicateHandle. */
enum { GH_KIND_NONE=0, GH_KIND_FILE=1, GH_KIND_EVENT=2, GH_KIND_MUTEX=3,
       GH_KIND_SEMAPHORE=4, GH_KIND_THREAD=5, GH_KIND_PROCESS=6,
       GH_KIND_FIND=7, GH_KIND_MAPPING=8, GH_KIND_TIMER=9, GH_KIND_OTHER=15 };
#define GH_FIRST_HANDLE 0x2000u
#define GH_MAX_HANDLES 65536u
typedef void (*GhDestroy)(void *object);
/* Returns new guest handle or 0 when the table is full. */
uint32_t gh_insert(uint32_t kind,void *object,GhDestroy destroy);
/* Returns host object if handle is live and of the expected kind (or any
 * kind when expected==GH_KIND_NONE); else NULL. *kind_out optional. */
void *gh_lookup(uint32_t handle,uint32_t expected,uint32_t *kind_out);
/* Adds a reference and returns a NEW handle to the same object (or 0). */
uint32_t gh_duplicate(uint32_t handle);
/* 1 = closed, 0 = invalid handle (caller sets ERROR_INVALID_HANDLE=6). */
int gh_close(uint32_t handle);
uint32_t gh_live_count(void);
void gh_reset_for_tests(void);
#endif
