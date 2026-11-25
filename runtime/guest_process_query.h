#ifndef BURNOUT_GUEST_PROCESS_QUERY_H
#define BURNOUT_GUEST_PROCESS_QUERY_H
#include "guest_memory.h"
#include "shim_loader.h"
/* Process enumeration/query bridges over the runtime-owned single guest
 * process. The port schedules exactly one guest process, so the enumerated
 * list is that process only; no host PID or captured Windows PID is reported.
 * Module names come from the registered host-shim loader, never from host
 * /proc or a PE introspection of an unregistered file.
 *
 * *result receives the Win32 return value; LastError is written to the guest
 * TEB. A non-zero function result is a model stop, not a Win32 status. */
#define GPQ_PROCESS_HANDLE 0x1f00u
#define GPQ_ERROR_INVALID_HANDLE 6u
#define GPQ_ERROR_INVALID_PARAMETER 87u
#define GPQ_MAIN_MODULE_NAME "BurnoutPR.exe"
/* OpenProcess over the runtime-owned process; other PIDs get the documented
 * ERROR_INVALID_PARAMETER refusal instead of a fabricated handle. */
int gpq_open_process(GuestMemory *,uint32_t teb,uint32_t access,uint32_t inherit,
                     uint32_t process,uint32_t *handle);
/* K32GetModuleBaseNameW: main image for module 0, else a loaded shim module. */
int gpq_module_base_name_w(GuestMemory *,const ShimLoader *,uint32_t process,
                           uint32_t module,uint32_t buffer,uint32_t capacity,uint32_t *result);
/* CloseHandle for the one synthetic process handle this runtime issues. */
int gpq_close_handle(GuestMemory *,uint32_t teb,uint32_t handle,uint32_t *result);
/* K32EnumProcesses: the single runtime-owned process, never host processes. */
int gpq_enumerate_processes(GuestMemory *,uint32_t teb,uint32_t output,uint32_t capacity,
                            uint32_t needed,uint32_t *result);
#endif
