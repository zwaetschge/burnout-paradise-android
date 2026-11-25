#ifndef BURNOUT_GUEST_SYNC_OBJECTS_H
#define BURNOUT_GUEST_SYNC_OBJECTS_H
#include "guest_memory.h"
/* Kernel event/mutex/semaphore objects for the ported guest.
 *
 * Object state lives in host memory and is reachable from the guest only as an
 * opaque handle issued by runtime/guest_handles.h (kinds GH_KIND_EVENT,
 * GH_KIND_MUTEX, GH_KIND_SEMAPHORE). No host pointer is ever serialized into
 * guest memory. Waits use real pthread condition variables, so a guest thread
 * started through guest_thread_api can block while another guest thread signals.
 *
 * Documented Win32 semantics are modelled: auto/manual-reset events, recursive
 * mutex ownership, semaphore counts with a maximum, INFINITE and millisecond
 * timeouts, WAIT_OBJECT_0/WAIT_ABANDONED/WAIT_TIMEOUT/WAIT_FAILED.
 *
 * Not modelled (explicit named stops instead of fabricated success):
 *  - named objects (lpName != NULL): the port has no cross-process namespace;
 *  - alertable waits (bAlertable != 0): no APC queue exists;
 *  - more than GSO_MAXIMUM_WAIT_OBJECTS handles;
 *  - waiting on a handle whose object was closed while the wait was pending.
 *
 * *result receives the Win32 return value; LastError is written to the guest
 * TEB (+0x34). A non-zero function result is a named model stop (GM_*). */
#define GSO_WAIT_OBJECT_0 0x00000000u
#define GSO_WAIT_ABANDONED 0x00000080u
#define GSO_WAIT_TIMEOUT 0x00000102u
#define GSO_WAIT_FAILED 0xffffffffu
#define GSO_INFINITE 0xffffffffu
#define GSO_MAXIMUM_WAIT_OBJECTS 64u
#define GSO_ERROR_INVALID_HANDLE 6u
#define GSO_ERROR_NOT_ENOUGH_MEMORY 8u
#define GSO_ERROR_INVALID_PARAMETER 87u
#define GSO_ERROR_NOT_OWNER 288u
#define GSO_ERROR_TOO_MANY_POSTS 298u

/* Registration list for the later win32_api_table wiring. Argument bytes are
 * the stdcall stack bytes consumed after the return address. */
typedef struct {const char *dll;const char *name;uint32_t stdcall_arg_bytes;void *fn;} GsoExport;
extern const GsoExport gso_exports[];
extern const uint32_t gso_export_count;

/* kernel32!CreateEventA/W. lpEventAttributes must be NULL (no security
 * descriptor model); lpName must be NULL (no named-object namespace). */
int gso_create_event(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t manual_reset,
                     uint32_t initial_state,uint32_t name,uint32_t *handle);
int gso_create_event_w(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t manual_reset,
                       uint32_t initial_state,uint32_t name,uint32_t *handle);
int gso_set_event(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
int gso_reset_event(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
/* kernel32!CreateMutexA/W. bInitialOwner != 0 makes the calling guest thread
 * (TEB+0x24) the owner with recursion 1, exactly like Win32. */
int gso_create_mutex(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t initial_owner,
                     uint32_t name,uint32_t *handle);
int gso_create_mutex_w(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t initial_owner,
                       uint32_t name,uint32_t *handle);
int gso_release_mutex(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
/* kernel32!CreateSemaphoreA/W: 0 < initial <= maximum, maximum >= 1. */
int gso_create_semaphore(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t initial,
                         uint32_t maximum,uint32_t name,uint32_t *handle);
int gso_create_semaphore_w(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t initial,
                           uint32_t maximum,uint32_t name,uint32_t *handle);
/* kernel32!ReleaseSemaphore: previous count is written to previous_count. */
int gso_release_semaphore(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t release_count,
                          uint32_t previous_count,uint32_t *result);
int gso_wait_single(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t milliseconds,uint32_t *result);
int gso_wait_single_ex(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t milliseconds,
                       uint32_t alertable,uint32_t *result);
int gso_wait_multiple(GuestMemory *,uint32_t teb,uint32_t handles,uint32_t count,uint32_t wait_all,
                      uint32_t milliseconds,uint32_t *result);
int gso_wait_multiple_ex(GuestMemory *,uint32_t teb,uint32_t handles,uint32_t count,uint32_t wait_all,
                         uint32_t milliseconds,uint32_t alertable,uint32_t *result);
/* InitializeCriticalSection / InitializeCriticalSectionAndSpinCount /
 * DeleteCriticalSection over the existing guest_critical.c object layout. */
int gso_initialize_critical_section(GuestMemory *,uint32_t section,uint32_t spin,uint32_t *result);
int gso_delete_critical_section(GuestMemory *,uint32_t section);
/* Thread-exit contract: a mutex owned by a terminating guest thread becomes
 * abandoned, so the next waiter receives WAIT_ABANDONED and takes ownership. */
int gso_abandon_thread(GuestMemory *,uint32_t thread);
/* Optional hook so WaitForSingleObject can wait on a GH_KIND_THREAD handle
 * without a hard module dependency; guest_thread_api installs it. Without a
 * hook, waiting on a thread handle is a named model stop. */
void gso_set_thread_wait_hook(int (*fn)(uint32_t handle,uint32_t milliseconds,uint32_t *result));
/* Diagnostics for tests: live object count of one kind (0 = all kinds). */
uint32_t gso_object_count(uint32_t kind);
void gso_reset_for_tests(void);
#endif
