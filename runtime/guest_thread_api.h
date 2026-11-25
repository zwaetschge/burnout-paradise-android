#ifndef BURNOUT_GUEST_THREAD_API_H
#define BURNOUT_GUEST_THREAD_API_H
#include "guest_memory.h"
/* Win32 thread API contract for the ported guest.
 *
 * A guest thread is a host-side object reachable only through a handle from
 * runtime/guest_handles.h (GH_KIND_THREAD). It stores the Win32 start address,
 * parameter, stack size and flags. The port never fabricates a guest CPU: the
 * AOT runtime installs a launcher with gta_set_thread_launcher() and that
 * launcher starts the guest thread. Without a launcher every attempt to start
 * a thread is the named stop GTA_STOP_NO_LAUNCHER, never a silent success.
 *
 * ExitThread() cannot return to the guest: it marks the thread exited, then
 * returns the named stop GTA_STOP_THREAD_EXIT so the runtime ends the guest
 * thread. A mutex owned by the exiting thread is abandoned, so a later waiter
 * receives WAIT_ABANDONED.
 *
 * Not modelled (named stop): SECURITY_ATTRIBUTES, suspending an already
 * running guest thread, alertable sleeps (no APC queue), Sleep(INFINITE),
 * thread priorities/affinity/desktop. */
#define GTA_STILL_ACTIVE 259u
#define GTA_CREATE_SUSPENDED 0x00000004u
#define GTA_ERROR_INVALID_HANDLE 6u
#define GTA_ERROR_NOT_ENOUGH_MEMORY 8u
#define GTA_ERROR_INVALID_PARAMETER 87u
/* Named model stops in this module's own range; GM_* stays the shared
 * guest-memory contract. */
enum { GTA_STOP_NO_LAUNCHER=0x100, GTA_STOP_THREAD_EXIT=0x101, GTA_STOP_INVALID_THREAD=0x102 };

typedef int (*GtaThreadLauncher)(uint32_t thread_id,uint32_t start,uint32_t parameter,
                                 uint32_t stack_size);
typedef void (*GtaThreadExitHook)(uint32_t thread_id,uint32_t exit_code);

typedef struct {const char *dll;const char *name;uint32_t stdcall_arg_bytes;void *fn;} GtaExport;
extern const GtaExport gta_exports[];
extern const uint32_t gta_export_count;

void gta_set_thread_launcher(GtaThreadLauncher fn);
void gta_set_thread_exit_hook(GtaThreadExitHook fn);
int gta_launcher_present(void);
/* Guest thread ids start at base (default 0x1008, step 4) so the runtime's
 * main thread id 0x1004 is never reused. */
void gta_set_thread_id_base(uint32_t base);
/* kernel32!CreateThread. thread_id is the guest address of the optional
 * lpThreadId out-parameter (0 = caller does not want it). */
int gta_create_thread(GuestMemory *,uint32_t teb,uint32_t attributes,uint32_t stack_size,
                      uint32_t start,uint32_t parameter,uint32_t flags,
                      uint32_t *handle,uint32_t thread_id);
int gta_resume_thread(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
int gta_suspend_thread(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
/* code is the guest address of the lpExitCode out-parameter (0 = optional). */
int gta_get_exit_code_thread(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t code,
                             uint32_t *result);
/* kernel32!ExitThread: marks the calling guest thread (TEB+0x24) exited. */
int gta_exit_thread(GuestMemory *,uint32_t teb,uint32_t exit_code,uint32_t *result);
int gta_sleep(uint32_t milliseconds,uint32_t *result);
int gta_sleep_ex(uint32_t milliseconds,uint32_t alertable,uint32_t *result);
int gta_switch_to_thread(uint32_t *result);
/* Runtime callbacks: the AOT runtime reports a finished guest thread here. */
int gta_note_thread_exit(uint32_t handle,uint32_t exit_code);
/* Wait hook installed into guest_sync_objects: WAIT_OBJECT_0 / WAIT_TIMEOUT. */
int gta_wait_thread(uint32_t handle,uint32_t milliseconds,uint32_t *result);
/* DuplicateHandle of the current-thread pseudo-handle: new real handle. */
int gta_open_current_thread(GuestMemory *,uint32_t teb,uint32_t *handle);
/* Thread `thread_id` has ended: every thread object of it is signalled. */
void gta_note_thread_exit_id(uint32_t thread_id,uint32_t exit_code);
uint32_t gta_thread_count(void);
void gta_reset_for_tests(void);
#endif
