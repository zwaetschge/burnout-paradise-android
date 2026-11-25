#ifndef BURNOUT_GUEST_KERNEL_MISC_H
#define BURNOUT_GUEST_KERNEL_MISC_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"
#include "guest_process_heap.h"
#include "guest_path_map.h"
#include "guest_locale.h"
#include "guest_string_compare.h"
#include "guest_environment.h"

/* A19 -- the kernel32/user32 semantics the static image was still missing.
 *
 * Everything here is modelled on the documented Win32 behaviour and on the
 * port's own already-verified projections; nothing is guessed:
 *
 *   GlobalAlloc/GlobalFree/GlobalLock/GlobalUnlock/GlobalSize/GlobalFlags/
 *   GlobalReAlloc and the Local* family
 *       Real blocks on the port-owned guest process heap
 *       (runtime/guest_process_heap.h). GMEM_FIXED returns the block address
 *       itself, GMEM_MOVEABLE returns a handle from the shared handle table
 *       (runtime/guest_handles.h, GH_KIND_OTHER) whose lock count, size and
 *       block address live in a host-side record. GlobalUnlock returns FALSE
 *       with LastError == NO_ERROR when the lock count reaches zero -- that is
 *       the documented Win32 quirk, not a failure.
 *   lstrlen/lstrcpy/lstrcpyn/lstrcat/lstrcmp/lstrcmpi A+W
 *       ANSI strings are CP1252 through the installed GuestLocale; a byte the
 *       locale cannot decode stops. lstrcmpiA folds through the captured
 *       sort-NLS casing map (the same table lstrcmpiW uses) by converting both
 *       operands to UTF-16 in guest scratch memory; without that table it stops
 *       instead of comparing with a host locale.
 *   GetSystemInfo/GetNativeSystemInfo
 *       PE32 SYSTEM_INFO (36 bytes): x86, page size 4096, minimum application
 *       address 0x10000, maximum 0x7FFEFFFF, allocation granularity 65536,
 *       processor count = online host cores clamped to 8 (documented below),
 *       active mask = (1 << count) - 1, processor type 586, level 6.
 *   GlobalMemoryStatus/GlobalMemoryStatusEx
 *       Host physical/available pages from sysconf; the virtual limits are the
 *       PE32 2 GiB user space and its free part. Documented as host-derived.
 *   GetVersionExA/W, VerifyVersionInfoA/W, VerSetConditionMask
 *       The declared compatibility version from port/guest-os-profile.json
 *       (10.0.19045, platform 2 = VER_PLATFORM_WIN32_NT, service pack 0,
 *       suite 0, product type 1 = VER_NT_WORKSTATION).
 *   GetComputerNameA/W, GetUserNameA/W
 *       The port's declared process identity: the user name is the USERNAME
 *       entry of the process environment profile (runtime/static_entry.c,
 *       "Player", the same name the path projection uses); the computer name is
 *       a port-owned constant (no Windows capture exists) and can be replaced
 *       with gkm_set_identity().
 *   MulDiv, IsDebuggerPresent (FALSE: no debugger is attached to this guest),
 *   IsWow64Process (FALSE: the guest is 32-bit x86 and the port models a 32-bit
 *   Windows), GetProcessAffinityMask, GetLogicalDrives, GetDiskFreeSpaceA,
 *   GetTempPathW, GetWindowsDirectoryW, GetLongPathNameW, SetEndOfFile,
 *   MoveFileW, RemoveDirectoryW, SetEnvironmentVariableA/W (overlay over the
 *   read-only process environment profile), OutputDebugStringA/W,
 *   GetUserPreferredUILanguages (en-US from the locale profile),
 *   SetThreadPriority/GetThreadPriority/SetThreadAffinityMask/
 *   SetThreadIdealProcessor (state only), GetExitCodeProcess,
 *   InterlockedPush/Pop/FlushEntrySList and QueryDepthSList (the PE32 8-byte
 *   SLIST_HEADER, updated atomically under a host spinlock).
 *
 * Named stops (never a fabricated success):
 *   - RaiseException: there is no SEH unwinder in this runtime, so the
 *     exception code and flags are reported and the port stops.
 *   - UnhandledExceptionFilter with an installed guest filter: calling the
 *     guest filter needs a guest-call hook that the static image does not have.
 *   - ExitProcess/TerminateProcess(self): an ordered runtime stop that reports
 *     the exit code; the harness decides what to do with it.
 *   - CreateProcessW: this runtime cannot start a child process, so the
 *     documented failure (FALSE + ERROR_FILE_NOT_FOUND) is returned, exactly
 *     like the Python model's --process-creation-refusal.
 *   - FormatMessageA, FindResourceA/LoadResource/LockResource/SizeofResource
 *     and the INI profile functions are *not* implemented here; they stay
 *     named stops because their semantics need data this port has not modelled
 *     (message tables, the PE resource tree, a guest INI store).
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. A non-zero
 * function return is a model stop, not a Win32 status. */

#define GKM_ERROR_SUCCESS 0u
#define GKM_ERROR_FILE_NOT_FOUND 2u
#define GKM_ERROR_PATH_NOT_FOUND 3u
#define GKM_ERROR_ACCESS_DENIED 5u
#define GKM_ERROR_INVALID_HANDLE 6u
#define GKM_ERROR_NOT_ENOUGH_MEMORY 8u
#define GKM_ERROR_INVALID_PARAMETER 87u
#define GKM_ERROR_INSUFFICIENT_BUFFER 122u
#define GKM_ERROR_ALREADY_EXISTS 183u
#define GKM_ERROR_FILENAME_EXCED_RANGE 206u
#define GKM_ERROR_DIR_NOT_EMPTY 145u
#define GKM_ERROR_ARITHMETIC_OVERFLOW 534u
#define GKM_ERROR_OLD_WIN_VERSION 1150u
#define GKM_ERROR_NOT_SUPPORTED 50u

#define GKM_STILL_ACTIVE 259u
#define GKM_MAXIMUM_PROCESSORS 32u

/* GlobalAlloc/LocalAlloc flags that are modelled. */
#define GKM_GMEM_FIXED 0x0000u
#define GKM_GMEM_MOVEABLE 0x0002u
#define GKM_GMEM_ZEROINIT 0x0040u
#define GKM_GMEM_SHARE 0x2000u
#define GKM_GHND 0x0042u
#define GKM_GPTR 0x0040u
#define GKM_GMEM_LOCKCOUNT 0x00ffu
/* Discardable/notify blocks have no equivalent in this runtime. */
#define GKM_GMEM_UNSUPPORTED 0x4100u /* DISCARDABLE | NOTIFY */
/* Documented as ignorable on the modern heap: NOCOMPACT | NODISCARD | LOWER. */
#define GKM_GMEM_IGNORED 0x1030u

/* Version projection (port/guest-os-profile.json). */
#define GKM_OS_MAJOR 10u
#define GKM_OS_MINOR 0u
#define GKM_OS_BUILD 19045u
#define GKM_OS_PLATFORM 2u          /* VER_PLATFORM_WIN32_NT */
#define GKM_OS_PRODUCT_TYPE 1u      /* VER_NT_WORKSTATION */
#define GKM_OSVERSIONINFOA_SIZE 148u
#define GKM_OSVERSIONINFOW_SIZE 276u
#define GKM_OSVERSIONINFOEXA_SIZE 156u
#define GKM_OSVERSIONINFOEXW_SIZE 284u
#define GKM_SYSTEM_INFO_SIZE 36u
#define GKM_MEMORYSTATUS_SIZE 32u
#define GKM_MEMORYSTATUSEX_SIZE 64u

/* VerifyVersionInfo type/condition bits. */
#define GKM_VER_MINORVERSION 0x0000001u
#define GKM_VER_MAJORVERSION 0x0000002u
#define GKM_VER_BUILDNUMBER 0x0000004u
#define GKM_VER_PLATFORMID 0x0000008u
#define GKM_VER_SERVICEPACKMINOR 0x0000010u
#define GKM_VER_SERVICEPACKMAJOR 0x0000020u
#define GKM_VER_SUITENAME 0x0000040u
#define GKM_VER_PRODUCT_TYPE 0x0000080u
#define GKM_VER_SUPPORTED_MASK 0x00000ffu
#define GKM_VER_EQUAL 1u
#define GKM_VER_GREATER 2u
#define GKM_VER_GREATER_EQUAL 3u
#define GKM_VER_LESS 4u
#define GKM_VER_LESS_EQUAL 5u
#define GKM_VER_AND 6u
#define GKM_VER_OR 7u

/* Thread priorities accepted by SetThreadPriority. */
#define GKM_THREAD_PRIORITY_IDLE (-15)
#define GKM_THREAD_PRIORITY_LOWEST (-2)
#define GKM_THREAD_PRIORITY_BELOW_NORMAL (-1)
#define GKM_THREAD_PRIORITY_NORMAL 0
#define GKM_THREAD_PRIORITY_ABOVE_NORMAL 1
#define GKM_THREAD_PRIORITY_HIGHEST 2
#define GKM_THREAD_PRIORITY_TIME_CRITICAL 15

/* PE32 SLIST_HEADER: Next(4) Depth(2) Sequence(2). */
#define GKM_SLIST_HEADER_SIZE 8u

typedef struct GuestKernelMisc GuestKernelMisc;

/* The heap, the path map, the locale, the string comparator and the process
 * environment are borrowed and must stay alive. Any of them may be NULL: the
 * entry points that need the missing one are then named stops. */
int gkm_create(GuestMemory *, GuestProcessHeap *, GuestPathMap *,
               const GuestLocale *, const GuestStringCompare *,
               const GuestEnvironmentEntry *, uint32_t env_count,
               GuestKernelMisc **out);
void gkm_destroy(GuestKernelMisc *);
/* LastError is written to the *current* guest thread's TEB, so the bridge
 * layer updates this before every call (guest threads have their own TEB). */
void gkm_set_teb(GuestKernelMisc *, uint32_t teb);
uint32_t gkm_teb(const GuestKernelMisc *);
/* Port-owned identity; overrides the defaults described in the header. */
int gkm_set_identity(GuestKernelMisc *, const char *computer, const char *user);

/* 64-bit return values (VerSetConditionMask): the bridge layer copies the
 * pending EDX into the guest register. Returns 1 when one is pending. */
int gkm_take_edx(GuestKernelMisc *, uint32_t *edx);

/* GetEnvironmentVariableA with the writable overlay in front of the read-only
 * process profile (runtime/static_bridges.c routes the bridge through here). */
int gkm_get_env_a(GuestKernelMisc *, uint32_t name, uint32_t buffer,
                  uint32_t capacity, uint32_t *result, uint32_t *error);

/* Diagnostics (host tests only). */
uint32_t gkm_global_blocks(const GuestKernelMisc *);
uint32_t gkm_moveable_handles(const GuestKernelMisc *);
uint32_t gkm_debug_strings(const GuestKernelMisc *);
uint32_t gkm_raise_count(const GuestKernelMisc *);
uint32_t gkm_exit_code(const GuestKernelMisc *);
uint32_t gkm_unhandled_filter(const GuestKernelMisc *);
uint32_t gkm_env_overrides(const GuestKernelMisc *);
uint32_t gkm_processor_count(const GuestKernelMisc *);
int gkm_thread_priority(const GuestKernelMisc *);
uint32_t gkm_affinity_mask(const GuestKernelMisc *);
uint32_t gkm_ideal_processor(const GuestKernelMisc *);
const char *gkm_last_log(const GuestKernelMisc *);

/* --- entry points (shared bridge signature) ---------------------------- */
int gkm_global_alloc(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_global_free(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_global_lock(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_global_unlock(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_global_size(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_global_flags(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_global_realloc(GuestKernelMisc *, const uint32_t *args, uint32_t *result);

int gkm_lstrlen_a(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrlen_w(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcpy_a(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcpy_w(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcpyn_a(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcpyn_w(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcat_a(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcat_w(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcmp_a(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcmp_w(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_lstrcmpi_a(GuestKernelMisc *, const uint32_t *args, uint32_t *result);

int gkm_get_system_info(GuestKernelMisc *, const uint32_t *args,
                        uint32_t *result);
int gkm_global_memory_status(GuestKernelMisc *, const uint32_t *args,
                             uint32_t *result);
int gkm_global_memory_status_ex(GuestKernelMisc *, const uint32_t *args,
                                uint32_t *result);
int gkm_get_version_ex_a(GuestKernelMisc *, const uint32_t *args,
                         uint32_t *result);
int gkm_get_version_ex_w(GuestKernelMisc *, const uint32_t *args,
                         uint32_t *result);
int gkm_verify_version_info_a(GuestKernelMisc *, const uint32_t *args,
                              uint32_t *result);
int gkm_verify_version_info_w(GuestKernelMisc *, const uint32_t *args,
                              uint32_t *result);
int gkm_ver_set_condition_mask(GuestKernelMisc *, const uint32_t *args,
                               uint32_t *result);
int gkm_mul_div(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_get_computer_name_a(GuestKernelMisc *, const uint32_t *args,
                            uint32_t *result);
int gkm_get_computer_name_w(GuestKernelMisc *, const uint32_t *args,
                            uint32_t *result);
int gkm_get_user_name_a(GuestKernelMisc *, const uint32_t *args,
                        uint32_t *result);
int gkm_get_user_name_w(GuestKernelMisc *, const uint32_t *args,
                        uint32_t *result);
int gkm_is_debugger_present(GuestKernelMisc *, const uint32_t *args,
                            uint32_t *result);
int gkm_is_wow64_process(GuestKernelMisc *, const uint32_t *args,
                         uint32_t *result);
int gkm_get_process_affinity_mask(GuestKernelMisc *, const uint32_t *args,
                                  uint32_t *result);
int gkm_get_logical_drives(GuestKernelMisc *, const uint32_t *args,
                           uint32_t *result);
int gkm_get_disk_free_space_a(GuestKernelMisc *, const uint32_t *args,
                              uint32_t *result);
int gkm_get_temp_path_w(GuestKernelMisc *, const uint32_t *args,
                        uint32_t *result);
int gkm_get_windows_directory_w(GuestKernelMisc *, const uint32_t *args,
                                uint32_t *result);
int gkm_get_long_path_name_w(GuestKernelMisc *, const uint32_t *args,
                             uint32_t *result);
int gkm_set_end_of_file(GuestKernelMisc *, const uint32_t *args,
                        uint32_t *result);
int gkm_move_file_w(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_remove_directory_w(GuestKernelMisc *, const uint32_t *args,
                           uint32_t *result);
int gkm_set_environment_variable_a(GuestKernelMisc *, const uint32_t *args,
                                   uint32_t *result);
int gkm_set_environment_variable_w(GuestKernelMisc *, const uint32_t *args,
                                   uint32_t *result);
int gkm_output_debug_string_a(GuestKernelMisc *, const uint32_t *args,
                              uint32_t *result);
int gkm_output_debug_string_w(GuestKernelMisc *, const uint32_t *args,
                              uint32_t *result);
int gkm_get_user_preferred_ui_languages(GuestKernelMisc *, const uint32_t *args,
                                        uint32_t *result);
int gkm_exit_process(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
int gkm_terminate_process(GuestKernelMisc *, const uint32_t *args,
                          uint32_t *result);
int gkm_get_exit_code_process(GuestKernelMisc *, const uint32_t *args,
                              uint32_t *result);
int gkm_raise_exception(GuestKernelMisc *, const uint32_t *args,
                        uint32_t *result);
int gkm_set_unhandled_exception_filter(GuestKernelMisc *, const uint32_t *args,
                                       uint32_t *result);
int gkm_unhandled_exception_filter(GuestKernelMisc *, const uint32_t *args,
                                   uint32_t *result);
int gkm_set_thread_priority(GuestKernelMisc *, const uint32_t *args,
                            uint32_t *result);
int gkm_get_thread_priority(GuestKernelMisc *, const uint32_t *args,
                            uint32_t *result);
int gkm_set_thread_affinity_mask(GuestKernelMisc *, const uint32_t *args,
                                 uint32_t *result);
int gkm_set_thread_ideal_processor(GuestKernelMisc *, const uint32_t *args,
                                   uint32_t *result);
int gkm_create_process_w(GuestKernelMisc *, const uint32_t *args,
                         uint32_t *result);
int gkm_interlocked_push_entry_slist(GuestKernelMisc *, const uint32_t *args,
                                     uint32_t *result);
int gkm_interlocked_pop_entry_slist(GuestKernelMisc *, const uint32_t *args,
                                    uint32_t *result);
int gkm_interlocked_flush_slist(GuestKernelMisc *, const uint32_t *args,
                                uint32_t *result);
int gkm_query_depth_slist(GuestKernelMisc *, const uint32_t *args,
                          uint32_t *result);

/* Registration list, wired by runtime/static_bridges.c. */
typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestKernelMisc *, const uint32_t *args, uint32_t *result);
} GuestKernelMiscExport;

extern const GuestKernelMiscExport kernel32_misc_exports[];
extern const uint32_t kernel32_misc_export_count;

#endif
