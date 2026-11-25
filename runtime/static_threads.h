#ifndef BURNOUT_STATIC_THREADS_H
#define BURNOUT_STATIC_THREADS_H
#include <stdint.h>
#include "guest_memory.h"

/* A14 -- guest threads for the static lift.
 *
 * Two things live here:
 *
 * 1. the *machine lock*. The lifted code is a direct translation of x86 that
 *    keeps its registers in RECOMP_TLS variables and reaches guest memory
 *    through raw host pointers. Two host threads inside lifted code at the same
 *    time would race on every guest byte the lifter did not translate into an
 *    atomic (upstream documents exactly this: "lifted code runs under one
 *    machine lock"). So exactly one host thread executes lifted code at a time;
 *    the lock is released around every blocking Win32 call (critical section
 *    contention, SRW contention, WaitForSingleObject, Sleep, thread join) and
 *    periodically from the upstream `recomp_yield_hook` back-edge counter, so a
 *    guest thread that spins in lifted code cannot starve the others.
 *
 *    This is a *serialisation* of guest CPUs, not a claim that the lifted code
 *    is lock-free. The Interlocked* APIs are real atomics (see
 *    runtime/static_bridges.c), so guest code that uses them is correct across
 *    threads even though the lifted `lock`-prefixed RMW sequence itself is
 *    serialised by the machine lock.
 *
 * 2. the thread launcher that runtime/guest_thread_api.c installs through
 *    gta_set_thread_launcher(). A guest thread is a real pthread with its own
 *    1 MiB guest stack, its own guest TEB (FS base, TEB.Self/StackBase/
 *    StackLimit/ClientId/LastError/StaticTLS vector) and its own RECOMP_TLS
 *    CPU state. The guest start routine is invoked as a Win32 stdcall with one
 *    argument; its EAX becomes the thread exit code and the thread handle
 *    becomes signalled through runtime/guest_sync_objects.c.
 *
 * The guest thread stack arena is 0x31000000..0x3F000000 (224 MiB), one
 * 0x110000-byte slot per thread: 1 MiB stack, 4 KiB TEB, 4 KiB static-TLS
 * vector. GuestVirtualAlloc (runtime/guest_virtual_memory.c) cannot be used for
 * them: the flat model maps the whole 32-bit address space once, so gvm's
 * per-reservation gm_map() would report GM_OVERLAP. That is documented, not
 * hidden.
 *
 * No host pointer is ever written into guest memory. */

#define ST_GUEST_STACK_SIZE   0x00100000u   /* 1 MiB, the modelled default */
#define ST_GUEST_STACK_ARENA  0x31000000u
#define ST_GUEST_STACK_LIMIT  0x3F000000u
#define ST_GUEST_SLOT_STRIDE  0x00110000u
#define ST_GUEST_MAX_THREADS  64u
#define ST_TEB_SIZE           0x00001000u
#define ST_TLS_VECTOR_SLOTS   64u

/* ---- machine lock ---- */
void st_machine_lock(void);
void st_machine_unlock(void);
/* Release the machine lock for the duration of a blocking call. Returns 1 when
 * the lock was actually held (pass the value back to st_machine_reacquire). */
int st_machine_release(void);
void st_machine_reacquire(int held);
int st_machine_held(void);
/* Installed as the upstream `recomp_yield_hook`: hands the machine to another
 * guest thread every RECOMP_YIELD_EVERY back edges. */
void st_machine_yield(void);
/* Sleep(0)/SwitchToThread: hand the machine to every other ready guest thread
 * first. 1 = another thread ran, 0 = nobody else was ready. */
int st_machine_switch(void);
/* Non-zero while a thread that woke from a wait is queued ahead of the
 * running one (cheap; the import thunk yields on it). */
int st_machine_preempt_pending(void);

/* ---- process context the launcher needs ---- */
/* Must be called once before any guest thread is started. `tls_template` is the
 * guest address of the main thread's static-TLS vector; every new thread gets a
 * copy of it (Win32 static TLS is per thread, initialised from the same
 * template). */
void st_thread_set_context(GuestMemory *memory, uint32_t peb, uint32_t process_id,
                           uint32_t tls_template, uint32_t tls_slots);
/* Named stop for a guest start address that is not translated. static_entry.c
 * installs it so the launcher reports the same PORT_TRAP as the main path. */
void st_thread_set_stop_hook(void (*fn)(const char *kind, uint32_t target));

/* gta_set_thread_launcher() callback. Returns 0 when the pthread was created. */
int st_thread_launch(uint32_t thread_id, uint32_t start, uint32_t parameter,
                     uint32_t stack_size);
/* Called by the CreateThread bridge once the Win32 handle is known, so the
 * launcher can signal it on exit (including a thread that already exited). */
void st_thread_bind_handle(uint32_t thread_id, uint32_t handle);

/* TEB of the calling host thread (0 when the caller is not a guest thread). */
uint32_t st_thread_teb(void);
uint32_t st_thread_id(void);
uint32_t st_thread_count(void);
/* Waits for every guest thread to finish (bounded by `milliseconds`). */
void st_thread_join_all(uint32_t milliseconds);

/* Writes the documented guest TEB layout that runtime/guest_peb.h and every
 * bridge use: +0x04 StackBase, +0x08 StackLimit, +0x14 Self, +0x20 ProcessId,
 * +0x24 ThreadId, +0x2c ThreadLocalStoragePointer, +0x30 PEB, +0x34 LastError,
 * +0x00 SEH chain end. Shared with the main-thread setup in static_entry.c so
 * the two layouts cannot drift apart. */
int st_teb_init(GuestMemory *memory, uint32_t teb, uint32_t self,
                uint32_t stack_base, uint32_t stack_limit, uint32_t process_id,
                uint32_t thread_id, uint32_t peb, uint32_t tls_vector);

#endif
