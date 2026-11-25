#ifndef BURNOUT_STATIC_BRIDGES_H
#define BURNOUT_STATIC_BRIDGES_H
#include <stdint.h>

#include "guest_memory.h"
#include "guest_file_api.h"
#include "guest_process_heap.h"
#include "guest_locale.h"
#include "guest_registry.h"
#include "guest_srw.h"
#include "guest_fls.h"
#include "guest_tls.h"
#include "guest_crt.h"
#include "guest_user32.h"
#include "guest_com.h"
#include "guest_string_compare.h"
#include "guest_winsock.h"
#include "shim_loader.h"
#include "guest_virtual_memory.h"
#include "local_heap.h"
#include "guest_commandline.h"
#include "guest_environment.h"

/* A14 -- one bridge table for the whole guest runtime.
 *
 * runtime/static_entry.c owns the process context (this header declares it) and
 * the import/thunk machinery; this module owns the *binding*: a single table
 * that merges every export list the runtime provides, so an import slot or a
 * GetProcAddress() name resolves to a real C bridge instead of a named stop.
 *
 * Sources wired here:
 *   kernel32_file_exports[]      (guest_file_api.c, A7)
 *   gso_exports[]                (guest_sync_objects.c, A6)
 *   gta_exports[]                (guest_thread_api.c, A6)
 *   gtc_exports[]                (guest_time_convert.c, A6)
 *   gvm_exports[]                (guest_virtual_memory.c, A6)
 *   gu_user32_exports[]          (guest_user32.c, A8)
 *   gu_com_exports[]             (guest_com.c, A8)
 *   crt_string/stdio/convert/heap/misc/vcruntime/msvcp_exports[]  (A11, __cdecl)
 *   advapi32 RegOpenKeyExW/RegQueryValueExW/RegCloseKey            (A3)
 *   SRW acquire/release/try                                        (A3)
 *   OpenProcess/K32EnumProcesses/K32GetModuleBaseNameW             (A3)
 *   lstrcmpiW                                                      (A3)
 *   WSACleanup                                                     (A3)
 * plus the small kernel32 bridges that need no extra module (TEB, heap, TLS,
 * LoadLibrary/GetProcAddress, code pages, clock, Interlocked*).
 *
 * Calling conventions
 * -------------------
 * stdcall (everything except the CRT): the thunk pops the return address plus
 * `argbytes`. The return value goes to EAX; `double` results go to ST0 through
 * the same x87 model the lifted code uses (see sb_invoke()).
 * __cdecl (the seven CRT tables): `argbytes` is 0, the caller pops, and the
 * bridge reads its arguments from the guest stack pointer itself
 * (GcrtInvoke(GuestCrt*, uint32_t sp, GcrtReturn*)).
 *
 * A bridge that cannot model an operation returns a GM_* / named stop code and
 * the caller turns that into a PORT_TRAP. No bridge ever returns a fabricated
 * Win32 success. */

/* ---- process context, owned by runtime/static_entry.c ---- */
extern GuestMemory *g_mem;
extern RECOMP_TLS uint32_t g_teb;      /* per guest thread */
extern GuestFileApi *g_fileapi;
extern GuestProcessHeap *g_heap;
extern GuestLocale g_locale;
extern GuestRegistry *g_registry;
extern GuestSrw g_srw;
extern GuestFls *g_fls;
extern RECOMP_TLS GuestTlsThread *g_tls_thread;   /* per guest thread */
extern ShimLoader g_loader;
extern GuestPathMap *g_pmap;
extern RECOMP_TLS GuestCrt *g_crt;     /* per guest thread (per-thread errno) */
extern GuestUser32 *g_user32;
extern GuestCom *g_com;
extern GuestStringCompare *g_strcmp;
extern GuestWinsock g_winsock;
extern GuestLocalHeap *g_local;          /* LocalAlloc/LocalFree arena */
extern GuestTlsProcess *g_tls_process;   /* dynamic-TLS slot allocator */
extern const GuestEnvironmentEntry *g_env_entries;
extern uint32_t g_env_count;
extern uint32_t g_peb;
extern uint32_t g_image_base;
extern uint32_t g_image_size;
/* Private 8-byte guest scratch cell (0x74007000) for bridges that need to hand
 * a value through guest memory (GetTickCount64's EDX:EAX). */
extern uint32_t g_scratch;

/* Lazily created per-thread CRT context / dynamic-TLS thread block. */
GuestCrt *sb_crt(void);
GuestTlsThread *sb_tls_thread(void);
/* Virtual-memory arena installed by runtime/static_entry.c (may be NULL). */
GuestVirtualMemory *sb_virtual_memory(void);
void sb_set_virtual_memory(GuestVirtualMemory *vm);

/* One registered bridge. Opaque to callers. */
typedef struct StaticBridge StaticBridge;

/* Build the table (idempotent). Returns the number of bridges. */
uint32_t sb_build(void);
uint32_t sb_count(void);
/* Lookup by DLL and function name (DLL name case-insensitive). NULL when the
 * pair is not provided by this runtime. */
const StaticBridge *sb_find(const char *dll, const char *name);
const char *sb_dll(const StaticBridge *b);
const char *sb_name(const StaticBridge *b);
uint32_t sb_argbytes(const StaticBridge *b);
/* 1 when the callee does not pop (__cdecl: the seven CRT tables). */
int sb_cdecl(const StaticBridge *b);

/* Invoke one bridge.
 *   esp    guest stack pointer of the call site: [esp] is the return address,
 *          argument i is at esp+4+4*i.
 *   *eax   Win32 return value.
 *   *edx   EDX for __int64 results (unchanged when the bridge has none).
 *   *st0   set to 1 and *st0_bits filled when the bridge returns a double.
 * Returns 0 or a GM_* / named stop code. */
int sb_invoke(const StaticBridge *b, uint32_t esp, uint32_t *eax, uint32_t *edx,
              int *st0_valid, uint64_t *st0_bits);

/* Diagnostics: how many bridges came from which family. */
typedef struct {
    uint32_t file, sync, thread, time, vmem, user32, com, crt, registry, srw,
             process_query, string_compare, winsock, kernel32;
} SbCensus;
void sb_census(SbCensus *out);

#endif
