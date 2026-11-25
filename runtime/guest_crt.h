#ifndef BURNOUT_GUEST_CRT_H
#define BURNOUT_GUEST_CRT_H
#include <stdint.h>
#include <stddef.h>
#include "guest_memory.h"
#include "guest_process_heap.h"
#include "guest_locale.h"
#include "guest_file_api.h"

/* 32-bit guest C runtime bridges: UCRT (api-ms-win-crt-*), vcruntime140 and
 * the imported msvcp140 C++ runtime entry points.
 *
 * Calling convention
 * ------------------
 * Every CRT entry point here is __cdecl: the caller pops the arguments, so
 * `stdcall_arg_bytes` is 0 for all of them. A bridge is invoked with the guest
 * stack pointer `sp` of the call site, i.e. [sp] = return address and argument
 * i lives at sp+4+4*i (8-byte arguments occupy two words, little endian).
 * Arguments are read through gm_read, never through a host pointer.
 *
 * Return values
 * -------------
 * Integer/pointer results come back in EAX (and EDX:EAX for __int64) and are
 * written to GcrtReturn.eax / .edx. `double` results (math, strtod, atof, ...)
 * come back on the x87 stack in ST0 under the 32-bit cdecl ABI. The guest CPU
 * state used by this port (nine words: GPR/EIP/EFLAGS) has no x87 registers,
 * so a bridge returns the IEEE-754 binary64 bit pattern in GcrtReturn.st0_bits
 * with st0_valid set. The dispatcher owns the x87 stack and must push it the
 * same way the upstream pcrecomp recomp32 model does
 * (port/upstream/pcrecomp/runtime/recomp32_cpu/cpu.h):
 *
 *     c->fpu_top = (c->fpu_top - 1) & 7;
 *     c->st[c->fpu_top] = <binary64 from st0_bits>;
 *
 * i.e. a pre-decrement push of the value as a host double. No other FPU state
 * (control word, status word, tag word) is touched by these bridges; the
 * modelled x87 stack stores doubles, exactly like the upstream model.
 *
 * Memory and errors
 * -----------------
 * Guest memory is only ever touched through gm_check/gm_read/gm_write. Win32
 * return values go through *result (or GcrtReturn), CRT error numbers go to the
 * guest `errno` int (see gcrt_errno_address), and a non-zero function return is
 * a named model stop (GM_*), never a Win32/CRT status. gcrt_stop_reason()
 * returns the text of the last named stop so a caller can log it.
 *
 * Documented model limits (all named stops, never silent zeros)
 * ------------------------------------------------------------
 *   - locale is the "C" locale: ctype/tolower/toupper/_strlwr/_stricmp are
 *     ASCII-only, exactly like UCRT without setlocale
 *   - no invalid parameter handler is installed; the *_s functions report
 *     EINVAL/ERANGE through errno and their return value like UCRT does when
 *     _invalid_parameter_handler is the default (which prints nothing here)
 *   - no C++ exception model: every msvcp140 thrower is a named stop carrying
 *     the mangled name and, where the import passes one, the guest message
 *   - no process creation: _popen/_pclose and exit() are named stops
 *   - stdio is unbuffered (setvbuf accepted and ignored); stdout/stderr of the
 *     GUI process are discarded, stdin reads EOF */

/* UCRT <errno.h> values used by these bridges. */
enum {
    GCRT_EPERM=1, GCRT_ENOENT=2, GCRT_ESRCH=3, GCRT_EINTR=4, GCRT_EIO=5,
    GCRT_EBADF=9, GCRT_EAGAIN=11, GCRT_ENOMEM=12, GCRT_EACCES=13,
    GCRT_EFAULT=14, GCRT_EBUSY=16, GCRT_EEXIST=17, GCRT_ENODEV=19,
    GCRT_ENOTDIR=20, GCRT_EISDIR=21, GCRT_EINVAL=22, GCRT_ENFILE=23,
    GCRT_EMFILE=24, GCRT_EFBIG=27, GCRT_ENOSPC=28, GCRT_ESPIPE=29,
    GCRT_EROFS=30, GCRT_EPIPE=32, GCRT_EDOM=33, GCRT_ERANGE=34,
    GCRT_ENAMETOOLONG=38, GCRT_ENOSYS=40, GCRT_ENOTEMPTY=41, GCRT_EILSEQ=42,
    GCRT_STRUNCATE=80
};

/* UCRT __stdio_common_* option bits. */
enum {
    GCRT_PF_LEGACY_VSPRINTF_NULL_TERMINATION=0x0001,
    GCRT_PF_STANDARD_SNPRINTF_BEHAVIOR=0x0002,
    GCRT_PF_LEGACY_WIDE_SPECIFIERS=0x0004,
    GCRT_PF_LEGACY_MSVCRT_COMPATIBILITY=0x0008,
    GCRT_PF_LEGACY_THREE_DIGIT_EXPONENTS=0x0010,
    GCRT_PF_STANDARD_ROUNDING=0x0020
};

typedef struct GuestCrt GuestCrt;

/* Guest callback hook used by qsort/bsearch/_initterm/_initterm_e. The
 * dispatcher runs the guest function at `address` with the given cdecl
 * arguments and reports the guest EAX. Without a hook those entry points are
 * named stops: the port never invents a comparison or an initializer result. */
typedef int (*GcrtGuestCall)(void *user,uint32_t address,const uint32_t *args,
                             uint32_t argc,uint32_t *eax);
/* Guest wall clock in seconds since 1970-01-01 UTC. */
typedef int64_t (*GcrtUnixSeconds)(void *user);

/* Result channel of one bridge call. */
typedef struct {
    uint32_t eax;
    uint32_t edx;
    uint64_t st0_bits;   /* IEEE-754 binary64, valid when st0_valid != 0 */
    uint8_t edx_valid;
    uint8_t st0_valid;
} GcrtReturn;

/* Uniform bridge entry: `sp` is the guest stack pointer of the call site. */
typedef int (*GcrtInvoke)(GuestCrt *crt,uint32_t sp,GcrtReturn *ret);

typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;   /* always 0: __cdecl, the caller pops */
    GcrtInvoke fn;
} GcrtExport;

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

/* `heap`/`heap_handle` are the process heap (runtime/guest_process_heap.h) used
 * for malloc and for the CRT's own guest bookkeeping blocks. `locale` and
 * `files` are borrowed and may be NULL; the entry points that need them then
 * stop instead of guessing. */
int gcrt_create(GuestMemory *memory,GuestProcessHeap *heap,uint32_t heap_handle,
                uint32_t teb,const GuestLocale *locale,GuestFileApi *files,
                GuestCrt **out);
void gcrt_destroy(GuestCrt *);
GuestMemory *gcrt_memory(GuestCrt *);
uint32_t gcrt_teb(const GuestCrt *);
uint32_t gcrt_heap_handle(const GuestCrt *);
const GuestLocale *gcrt_locale(const GuestCrt *);
GuestFileApi *gcrt_files(GuestCrt *);

int gcrt_set_guest_call(GuestCrt *,GcrtGuestCall fn,void *user);
int gcrt_set_clock(GuestCrt *,GcrtUnixSeconds fn,void *user);
/* Windows TIME_ZONE_INFORMATION.Bias in minutes (UTC - local). Default 0. */
int gcrt_set_timezone_bias(GuestCrt *,int32_t bias_minutes);
int32_t gcrt_timezone_bias(const GuestCrt *);
/* Guest address of the CRT `errno` int. 0 selects a lazily allocated private
 * 4-byte block in the guest heap. */
int gcrt_set_errno_address(GuestCrt *,uint32_t address);
int gcrt_errno_address(GuestCrt *,uint32_t *address);
int gcrt_errno(const GuestCrt *,uint32_t *value);
int gcrt_set_errno(GuestCrt *,uint32_t value);

/* Named stop: records `reason` (a static string) and returns GM_UNSUPPORTED. */
int gcrt_stop(GuestCrt *,const char *reason);
const char *gcrt_stop_reason(const GuestCrt *);

/* ------------------------------------------------------------------ */
/* Shared helpers (used by the per-module bridge files)                */
/* ------------------------------------------------------------------ */

/* Argument i of a cdecl call at `sp`: value at sp+4+4*i. */
int gcrt_arg32(GuestCrt *,uint32_t sp,uint32_t index,uint32_t *value);
int gcrt_arg64(GuestCrt *,uint32_t sp,uint32_t index,uint64_t *value);

/* Bounded guest string reads. *length receives the number of units before the
 * terminator; a missing terminator inside `capacity` is GM_RANGE. */
int gcrt_read_ansi(GuestCrt *,uint32_t address,char *out,uint32_t capacity,
                   uint32_t *length);
int gcrt_read_wide(GuestCrt *,uint32_t address,uint16_t *out,uint32_t capacity,
                   uint32_t *length);
/* Writes text plus terminator; GM_RANGE when capacity is too small. */
int gcrt_write_ansi(GuestCrt *,uint32_t address,const char *text,uint32_t capacity);
int gcrt_write_wide(GuestCrt *,uint32_t address,const uint16_t *text,
                    uint32_t capacity);
/* Length of a guest string, bounded by `limit` units (GM_RANGE when no
 * terminator is found inside the limit). */
int gcrt_strlen_ansi(GuestCrt *,uint32_t address,uint32_t limit,uint32_t *length);
int gcrt_strlen_wide(GuestCrt *,uint32_t address,uint32_t limit,uint32_t *length);

/* Process-heap helpers over gcrt's heap (flags: 8 = zero memory). */
int gcrt_heap_alloc(GuestCrt *,uint32_t flags,uint32_t size,uint32_t *address);
int gcrt_heap_free(GuestCrt *,uint32_t address);
int gcrt_heap_size(GuestCrt *,uint32_t address,uint32_t *size);
int gcrt_heap_realloc(GuestCrt *,uint32_t flags,uint32_t address,uint32_t size,
                      uint32_t *result);

/* Per-context guest scratch blocks (lazily allocated from the process heap). */
enum { GCRT_SLOT_ERRNO=0, GCRT_SLOT_TM=1, GCRT_SLOT_STRERROR=2,
       GCRT_SLOT_ECVT=3, GCRT_SLOT_WCSTOK=4, GCRT_SLOT_IO=5,
       GCRT_SLOT_IOBUF=6, GCRT_SLOT_COUNT=7 };
int gcrt_scratch(GuestCrt *,uint32_t slot,uint32_t size,uint32_t *address);
/* stdio FILE* slots for stdin/stdout/stderr (owned by guest_crt_stdio.c). */
uint32_t *gcrt_std_file_slot(GuestCrt *,uint32_t index);
uint32_t *gcrt_tmpfile_counter(GuestCrt *);
uint32_t *gcrt_wcstok_cursor(GuestCrt *);
uint32_t *gcrt_rand_state(GuestCrt *);
GcrtGuestCall gcrt_guest_call(GuestCrt *,void **user);
/* Injected clock, else the host CLOCK_REALTIME in seconds. */
int64_t gcrt_now_seconds(GuestCrt *);
int64_t gcrt_host_seconds(void);

/* CP1252 (ANSI) <-> UTF-16 through the installed GuestLocale. */
int gcrt_ansi_to_wide(GuestCrt *,const char *text,uint32_t units,uint16_t *out,
                      uint32_t capacity,uint32_t *written);
int gcrt_wide_to_ansi(GuestCrt *,const uint16_t *text,uint32_t units,char *out,
                      uint32_t capacity,uint32_t *written);

/* ------------------------------------------------------------------ */
/* Per-module registration lists (wired into runtime/win32_api_table.c  */
/* later; that file and runtime/startup_host.c are not touched here).   */
/* ------------------------------------------------------------------ */
extern const GcrtExport crt_string_exports[];
extern const uint32_t crt_string_export_count;
extern const GcrtExport crt_stdio_exports[];
extern const uint32_t crt_stdio_export_count;
extern const GcrtExport crt_convert_exports[];
extern const uint32_t crt_convert_export_count;
extern const GcrtExport crt_heap_exports[];
extern const uint32_t crt_heap_export_count;
extern const GcrtExport crt_misc_exports[];
extern const uint32_t crt_misc_export_count;
extern const GcrtExport vcruntime_exports[];
extern const uint32_t vcruntime_export_count;
extern const GcrtExport msvcp_exports[];
extern const uint32_t msvcp_export_count;

/* Lookup by DLL!Function across all CRT tables (case-insensitive DLL, exact
 * function name). NULL when the pair is not implemented here. */
const GcrtExport *gcrt_lookup(const char *dll,const char *name);
/* Total number of implemented CRT entry points. */
uint32_t gcrt_export_total(void);

#endif
