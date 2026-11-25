/* A10/A14 -- process entry for the full static lift.
 *
 * This is the integration layer between three things that already existed
 * separately:
 *
 *   1. the generated recomp32 code (port/generated/static-full-v1, produced by
 *      tools/static_full_lift.py) which addresses guest memory through the
 *      upstream ADDR()/MEM32() macros and reaches imports through
 *      RECOMP_ICALL(slot_va) -> recomp_lookup_import(),
 *   2. the project's guest runtime (runtime/guest_*.c) which models Win32 on a
 *      4 KiB-page GuestMemory,
 *   3. the import manifest runtime/win32_api_table.c (A4) which names every
 *      declared import and stops on the ones that have no bridge.
 *
 * Memory model (the one decision that makes the two halves fit):
 *   The lifted code cannot afford a page-table lookup per guest access, so it
 *   dereferences guest addresses directly. We therefore reserve one flat 4 GiB
 *   host region, publish its base as g_mem_base, and hand the *same* bytes to
 *   GuestMemory as a single RWX mapping. gm_read/gm_write then operate on the
 *   identical bytes the lifted code sees, so every existing bridge
 *   (guest_file_api, guest_peb, guest_process_heap, ...) works unmodified.
 *   Documented consequence: the flat host run has no per-page protection
 *   model, so a bridge's GM_ACCESS check can never fire. That is fine for a
 *   bring-up run and is called out in the report; it is not a Win32 claim.
 *
 * A14: the bridge table moved to runtime/static_bridges.c, which registers
 * every export list the runtime provides (file API, sync objects, thread API,
 * time, virtual memory, user32, COM, the seven CRT tables, registry, SRW,
 * process query, lstrcmpiW, WSACleanup) plus one CloseHandle dispatcher. This
 * file keeps the process context, the PE loader, the IAT/thunk machinery and
 * the named stops.
 *
 * Nothing is faked. Every import either has a real C bridge or ends in
 * named_stop("GUEST_API_UNIMPLEMENTED dll!name"). Every dispatch target that
 * was not translated (the whole .trace section in v1) ends in
 * named_stop("trace-target 0x...") with the guest function and the indirect
 * call chain.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "recomp_types.h"

#include "static_entry.h"
#include "static_boot.h"
#include "static_bridges.h"
#include "static_threads.h"
#include "guest_memory.h"
#include "guest_handles.h"
#include "guest_locale.h"
#include "guest_path_map.h"
#include "guest_file_api.h"
#include "guest_peb.h"
#include "guest_process_heap.h"
#include "guest_environment.h"
#include "guest_commandline.h"
#include "guest_static_tls.h"
#include "guest_critical.h"
#include "guest_slist.h"
#include "guest_condition.h"
#include "guest_srw.h"
#include "guest_fls.h"
#include "guest_tls.h"
#include "guest_clock.h"
#include "guest_registry.h"
#include "guest_string_compare.h"
#include "d3d9_thunk.h"
#include "guest_dinput.h"
#include "android_input.h"
#include "orig_bridges.h"
void st_thread_exit_current(void);   /* runtime/static_threads.c */
#include "ordinal_names.h"
#include "guest_winsock.h"
#include "guest_user32.h"
#include "guest_com.h"
#include "guest_virtual_memory.h"
#include "guest_sync_objects.h"
#include "guest_thread_api.h"
#include "shim_loader.h"
#include "host_bridges.h"
#include "guest_system_time.h"
#include "win32_api_table.h"

/* ------------------------------------------------------------------ */
/* guest layout (mirrors runtime/startup_host.c setup() so the snapshot
 * lineage and the static image describe the same process)              */
/* ------------------------------------------------------------------ */
#define IMAGE_BASE        0x00400000u
#define STACK_BASE        0x30000000u
#define STACK_SIZE        0x00100000u
#define STACK_TOP         0x30080000u
#define TEB_BASE          0x7FFDE000u
#define COMMAND_BASE      0x74000000u
#define MODULE_POOL_BASE  0x71000000u
#define TLS_VECTOR_BASE   0x76000000u
#define TLS_VECTOR_SLOTS  64u
#define SCRATCH_BASE      0x74007000u
#define THUNK_BASE        0x60000000u
#define THUNK_STRIDE      16u
/* 384 declared IAT slots plus every name/ordinal the game resolves through
 * GetProcAddress (the .srdata delay-load resolver asks for ~250, and a real
 * run asks for more). Thunk space: 0x60000000 + 2048*16 = 0x60008000. */
#define MAX_IMPORTS       2048u
#define FLAT_SIZE         0x100000000ull   /* the whole 32-bit guest space */
#define VIRTUAL_ARENA_BASE 0x40000000u
#define VIRTUAL_ARENA_LIMIT 0x50000000u

#define PORT_TRAP_EXIT    70

static uint8_t *g_flat;             /* the 4 GiB reservation */

/* ---- process context shared with runtime/static_bridges.c ---- */
GuestMemory *g_mem;
RECOMP_TLS uint32_t g_teb = TEB_BASE;
GuestFileApi *g_fileapi;
GuestProcessHeap *g_heap;
GuestLocale g_locale;
GuestRegistry *g_registry;
GuestSrw g_srw;
GuestFls *g_fls;
RECOMP_TLS GuestTlsThread *g_tls_thread;
ShimLoader g_loader;
GuestPathMap *g_pmap;
RECOMP_TLS GuestCrt *g_crt;
GuestUser32 *g_user32;
GuestCom *g_com;
GuestStringCompare *g_strcmp;
GuestWinsock g_winsock;
uint32_t g_peb = GP_BASE;
uint32_t g_image_base = IMAGE_BASE;
uint32_t g_image_size;
uint32_t g_scratch = SCRATCH_BASE;

GuestLocalHeap *g_local;
GuestTlsProcess *g_tls_process;
static ShimModule g_modules[64];
static uint32_t g_module_refs[64];
static size_t g_module_count;
static uint32_t g_entry_va;
static uint32_t g_last_eip, g_last_esp, g_last_func;

/* ------------------------------------------------------------------ */
/* named trap                                                          */
/* ------------------------------------------------------------------ */

static void json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; s && *s && o + 2 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

/* Print one machine-readable trap line plus the indirect call chain, then
 * terminate. Never returns, never fabricates a value. */
#ifdef PORT_PCPU
/* PF engine (runtime/static_pcpu.c): the guest registers live in a per-thread
 * CPU struct while guest code runs; these copy them to/from the g_e* globals
 * the bridge layer uses. */
void pf_to_globals(void);
void pf_from_globals(void);
uint32_t pf_eip(void);
void pf_set_eip(uint32_t va);
void pf_dispatch_trace(unsigned long long n);
extern RECOMP_TLS int g_pf_live;
#endif
/* Trap report output: stderr (the runtime log) and, once opened, the trap
 * detail file. */
static int g_trap_fd = -1;
static void trap_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void trap_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_trap_fd >= 0) {
        va_start(ap, fmt);
        vdprintf(g_trap_fd, fmt, ap);
        va_end(ap);
    }
}
static void named_stop(const char *kind, const char *dll, const char *name,
                       uint32_t target) __attribute__((noreturn));
static void named_stop(const char *kind, const char *dll, const char *name,
                       uint32_t target) {
    char edll[96], ename[192];
    json_escape(dll, edll, sizeof edll);
    json_escape(name, ename, sizeof ename);
#ifdef PORT_PCPU
    if (g_pf_live) { pf_to_globals(); g_cur_func = pf_eip(); }
#endif
    g_last_eip = g_cur_func;
    g_last_esp = g_esp;
    g_last_func = g_cur_func;
    /* The guest return address on top of the stack: for an import thunk that is
     * the exact instruction in the lifted code that made the call, which is
     * what makes a frontier reproducible. */
    uint32_t caller = 0;
    if (g_esp) gm_read(g_mem, g_esp, &caller, 4);
    /* Everything below also goes straight into a file next to the shader
     * cache (PORT_TRAP_DETAIL overrides): on the device the stderr pipe has
     * twice lost every line after the first one. */
    {
        const char *detail = getenv("PORT_TRAP_DETAIL"), *cache = getenv("PORT_SHADER_CACHE");
        char path[1024] = "";
        if (detail && *detail) snprintf(path, sizeof path, "%s", detail);
        else if (cache && *cache) {
            snprintf(path, sizeof path, "%s", cache);
            char *slash = strrchr(path, '/');
            if (slash) snprintf(slash, sizeof path - (size_t)(slash - path), "/port-trap-detail.txt");
            else path[0] = 0;
        }
        if (path[0]) {
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
            if (fd >= 0) g_trap_fd = fd;
        }
    }
    trap_printf("PORT_TRAP {\"kind\":\"%s\",\"eip\":\"0x%08X\",\"esp\":\"0x%08X\","
            "\"target\":\"0x%08X\",\"dll\":\"%s\",\"name\":\"%s\","
            "\"caller\":\"0x%08X\",\"icalls\":%u}\n",
            kind, g_cur_func, g_esp, target, edll, ename, caller, g_icall_count);
    if (!strcmp(kind, "unlifted-target") && g_flat) {
        /* A transfer to something that is not translated code: show what is at
         * the target and around the register operands (a corrupted callback
         * record looks different from generated code). */
        const uint32_t where[4] = { target, g_eax, g_esi, g_esp };
        static const char *const label[4] = { "target", "eax", "esi", "esp" };
        /* Checked reads: the target may lie in released guest memory, and the
         * diagnostic must not fault where the guest did not. */
        for (int k = 0; k < 4; k++) {
            if (where[k] < 0x10000u || where[k] > 0xFFFFFF00u) continue;
            trap_printf("PORT_TRAP_MEM %s=0x%08X:", label[k], where[k]);
            for (uint32_t i = 0; i < 48; i += 4) {
                uint32_t v;
                if (gm_read(g_mem, where[k] + i, &v, 4) == GM_OK) trap_printf(" %08X", v);
                else { trap_printf(" (unmapped)"); break; }
            }
            trap_printf("\n");
        }
    }
    if (g_esp) {
        /* Return-address candidates on the guest stack (values inside the
         * image's code): the call chain of the stopping function. */
        trap_printf("PORT_TRAP_STACK");
        unsigned shown = 0;
        for (uint32_t i = 0; i < 1024 && shown < 24; i += 4) {
            uint32_t v;
            if (gm_read(g_mem, g_esp + i, &v, 4) != GM_OK) break;
            if (v >= 0x00401000u && v < 0x00A2F000u) { trap_printf(" +%X:0x%08X", i, v); shown++; }
        }
        trap_printf("\n");
    }
    {
        trap_printf("PORT_TRAP_STATE dispatches=%llu next=0x%08X eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X\n",
                (unsigned long long)g_dispatch_count, g_next, g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp);
    }
    /* The last 16 indirect transfers: target <- caller. This is the call chain
     * that led to the missing piece. */
    uint32_t n = g_icall_trace_idx < ICALL_TRACE_SIZE ? g_icall_trace_idx
                                                     : ICALL_TRACE_SIZE;
    trap_printf("PORT_TRAP_CHAIN");
    for (uint32_t i = 0; i < n && i < 16; i++) {
        uint32_t k = (g_icall_trace_idx - 1 - i) & (ICALL_TRACE_SIZE - 1);
        trap_printf(" 0x%08X<-0x%08X", g_icall_trace[k], g_icall_from[k]);
    }
    trap_printf("\n");
    fflush(stderr);
    fflush(stdout);
    if (g_trap_fd >= 0) { fsync(g_trap_fd); close(g_trap_fd); }
    _exit(PORT_TRAP_EXIT);
}

/* Stop hook for a guest thread whose start address is not translated. */
static void thread_stop(const char *kind, uint32_t target) {
    named_stop(kind, NULL, NULL, target);
}

/* ------------------------------------------------------------------ */
/* flat guest memory                                                   */
/* ------------------------------------------------------------------ */

static int flat_reserve(void) {
    /* One reservation for the entire 32-bit guest address space. MAP_NORESERVE
     * keeps it a pure virtual reservation (pages materialise on first touch),
     * so the 4 GiB costs no RAM and no swap commitment. */
#ifdef RECOMP_MEM_BASE
    /* The lifted code was built with the guest base as a constant (no global
     * to load on every memory access), so the reservation has to be exactly
     * there. MAP_FIXED_NOREPLACE fails instead of displacing another mapping. */
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    void *want = (void *)(uintptr_t)(RECOMP_MEM_BASE);
    void *p = mmap(want, (size_t)FLAT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED && p != want) {             /* a kernel without NOREPLACE treats it as a hint */
        munmap(p, (size_t)FLAT_SIZE);
        p = MAP_FAILED;
        errno = EEXIST;
    }
    if (p == MAP_FAILED) {
        fprintf(stderr, "PORT_FATAL flat reservation of 4 GiB at fixed base %p failed: %s\n",
                want, strerror(errno));
        return -1;
    }
#else
    void *p = mmap(NULL, (size_t)FLAT_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "PORT_FATAL flat reservation of 4 GiB failed: %s\n",
                strerror(errno));
        return -1;
    }
#endif
    g_flat = (uint8_t *)p;
    g_mem_base = (ptrdiff_t)(uintptr_t)p;
    return 0;
}

static int guest_memory_attach(void) {
    g_mem = gm_create();
    if (!g_mem) return -1;
    /* Same bytes as the lifted code sees. Two calls because gm_map takes a
     * 32-bit size and 0x100000000 does not fit; both halves are page aligned. */
    if (gm_map(g_mem, 0x00000000u, 0xFFF00000u, g_flat, 1u, 0x40u)) return -1;
    if (gm_map(g_mem, 0xFFF00000u, 0x00100000u, g_flat + 0xFFF00000u, 2u, 0x40u))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* PE image load                                                       */
/* ------------------------------------------------------------------ */

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

typedef struct {
    char name[16];
    uint32_t va, vsize, raw_size, raw_ptr, characteristics;
} PeSection;

static PeSection g_sections[32];
static uint32_t g_section_count;
static uint32_t g_import_dir_va, g_import_dir_size;

static uint8_t *read_whole_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n <= 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size_out = (size_t)n;
    return buf;
}

/* Load <game_root>/BurnoutPR.exe section for section at IMAGE_BASE. Read-only
 * access to the file; the original is never written. */
static int load_image(const char *game_root) {
    char path[4096];
    snprintf(path, sizeof path, "%s/BurnoutPR.exe", game_root);
    size_t size = 0;
    uint8_t *file = read_whole_file(path, &size);
    if (!file) {
        fprintf(stderr, "PORT_FATAL cannot read game image %s: %s\n", path,
                strerror(errno));
        return -1;
    }
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z') {
        fprintf(stderr, "PORT_FATAL %s is not a PE image\n", path);
        free(file);
        return -1;
    }
    uint32_t pe = rd32(file + 0x3c);
    if (pe + 24 > size || memcmp(file + pe, "PE\0\0", 4)) {
        fprintf(stderr, "PORT_FATAL %s has no PE signature\n", path);
        free(file);
        return -1;
    }
    const uint8_t *coff = file + pe + 4;
    uint32_t nsections = rd16(coff + 2);
    uint32_t opt_size = rd16(coff + 16);
    const uint8_t *opt = coff + 20;
    if (rd16(opt) != 0x10b) {
        fprintf(stderr, "PORT_FATAL %s is not PE32\n", path);
        free(file);
        return -1;
    }
    uint32_t image_base = rd32(opt + 28);
    g_image_size = rd32(opt + 56);
    g_entry_va = image_base + rd32(opt + 16);
    uint32_t size_of_headers = rd32(opt + 60);
    uint32_t ndirs = rd32(opt + 92);
    if (image_base != IMAGE_BASE) {
        fprintf(stderr, "PORT_FATAL image base 0x%08X, the lift assumes 0x%08X\n",
                image_base, IMAGE_BASE);
        free(file);
        return -1;
    }
    if (ndirs > 1) {
        g_import_dir_va = rd32(opt + 96 + 8);
        g_import_dir_size = rd32(opt + 96 + 12);
    }
    const uint8_t *sh = opt + opt_size;
    g_section_count = 0;
    for (uint32_t i = 0; i < nsections && i < 32; i++, sh += 40) {
        PeSection *s = &g_sections[g_section_count++];
        memcpy(s->name, sh, 8);
        s->name[8] = 0;
        for (int k = 7; k >= 0 && s->name[k] == 0; k--) s->name[k] = 0;
        s->vsize = rd32(sh + 8);
        s->va = rd32(sh + 12);
        s->raw_size = rd32(sh + 16);
        s->raw_ptr = rd32(sh + 20);
        s->characteristics = rd32(sh + 36);
    }
    /* headers */
    if (size_of_headers > size) size_of_headers = (uint32_t)size;
    memcpy(g_flat + IMAGE_BASE, file, size_of_headers);
    for (uint32_t i = 0; i < g_section_count; i++) {
        PeSection *s = &g_sections[i];
        uint8_t *dst = g_flat + IMAGE_BASE + s->va;
        uint32_t vfill = s->vsize ? s->vsize : s->raw_size;
        memset(dst, 0, vfill);
        uint32_t copy = s->raw_size;
        if (copy > vfill) copy = vfill;
        if (s->raw_ptr + copy > size) {
            fprintf(stderr, "PORT_FATAL section %s raw range outside the file\n",
                    s->name);
            free(file);
            return -1;
        }
        memcpy(dst, file + s->raw_ptr, copy);
    }
    free(file);
    fprintf(stderr,
            "PORT_IMAGE base=0x%08X size=0x%08X entry=0x%08X sections=%u\n",
            image_base, g_image_size, g_entry_va, g_section_count);
    for (uint32_t i = 0; i < g_section_count; i++)
        fprintf(stderr, "PORT_SECTION %-8s va=0x%08X vsize=0x%08X raw=0x%08X\n",
                g_sections[i].name, IMAGE_BASE + g_sections[i].va,
                g_sections[i].vsize, g_sections[i].raw_size);
    return 0;
}

/* Name of the section that contains `va`, or NULL. Used only for diagnostics. */
static const char *section_of(uint32_t va) {
    if (va < IMAGE_BASE || va >= IMAGE_BASE + g_image_size) return NULL;
    for (uint32_t i = 0; i < g_section_count; i++) {
        uint32_t lo = IMAGE_BASE + g_sections[i].va;
        uint32_t hi = lo + (g_sections[i].vsize ? g_sections[i].vsize
                                                : g_sections[i].raw_size);
        if (va >= lo && va < hi) return g_sections[i].name;
    }
    return NULL;
}

/* runtime/static_bridges.c: create the A16/A19 module contexts. Declared here
 * (rather than in runtime/static_bridges.h) so the shared header keeps its A14
 * shape for the other modules. */
int sb_initialize_contexts(const char *user_root);

/* ------------------------------------------------------------------ */
/* import table + thunks                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t slot;      /* guest VA of the IAT slot (0 for a dynamic thunk) */
    uint32_t thunk;     /* guest VA of its unique thunk */
    char dll[64];
    char name[128];
    uint32_t ordinal;
    uint32_t argbytes;
    int bound;
    const StaticBridge *bridge;
    /* A4 manifest row (runtime/win32_api_table.c) whose `run_*` adapter lives
     * in runtime/host_bridges.c. The adapter consumes its own stdcall frame,
     * so no argbytes are needed here. */
    const GuestApiEntry *a4;
} StaticImport;

static StaticImport g_imports[MAX_IMPORTS];
static uint32_t g_import_count;
#define g_pending_import (g_cpu.c_pending_import)   /* per thread, starts at -1 */
/* Export-resolution policy, see port_set_resolve_unknown_exports(). */
static int g_resolve_unknown = 1;
static int g_trace_exports;

static int import_cstr(uint32_t rva, char *out, size_t cap) {
    if (rva >= g_image_size) return -1;
    const char *src = (const char *)(g_flat + IMAGE_BASE + rva);
    size_t i = 0;
    for (; i + 1 < cap && src[i]; i++) out[i] = src[i];
    out[i] = 0;
    return i ? 0 : -1;
}

/* Guest address of an import's entry point. With the oracle boot registry
 * (PORT_BOOT) this is the registry's export address -- inside the mapped
 * NTDLL/Kernel32 views or the trap pool -- exactly as the proven startup model
 * hands it to the guest; otherwise a runtime thunk slot. */
static uint32_t import_entry_address(const char *dll, const char *name, uint32_t index) {
    if (static_boot_loaded()) {
        uint32_t va = static_boot_export_address(dll, name);
        if (va) return va;
    }
    return THUNK_BASE + index * THUNK_STRIDE;
}

static int parse_imports(void) {
    if (!g_import_dir_va) {
        fprintf(stderr, "PORT_FATAL image has no import directory\n");
        return -1;
    }
    uint32_t desc = IMAGE_BASE + g_import_dir_va;
    for (uint32_t d = 0;; d++) {
        const uint8_t *p = g_flat + desc + d * 20;
        /* IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk, TimeDateStamp,
         * ForwarderChain, Name, FirstThunk -- Name is +12, FirstThunk +16. */
        uint32_t ilt = rd32(p), name_rva = rd32(p + 12), ts = rd32(p + 16);
        if (!ilt && !ts && !name_rva) break;
        if (d > 64) { fprintf(stderr, "PORT_FATAL import descriptors runaway\n"); return -1; }
        char dll[64];
        if (import_cstr(name_rva, dll, sizeof dll)) {
            fprintf(stderr, "PORT_FATAL bad import DLL name\n");
            return -1;
        }
        if (!ts) continue;
        for (uint32_t k = 0;; k++) {
            uint32_t entry = rd32(g_flat + IMAGE_BASE + (ilt ? ilt : ts) + k * 4);
            if (!entry) break;
            if (g_import_count >= MAX_IMPORTS) {
                fprintf(stderr, "PORT_FATAL more than %u imports\n", MAX_IMPORTS);
                return -1;
            }
            StaticImport *im = &g_imports[g_import_count];
            memset(im, 0, sizeof *im);
            snprintf(im->dll, sizeof im->dll, "%s", dll);
            if (entry & 0x80000000u) {
                im->ordinal = entry & 0xffffu;
                /* A19: an ordinal import has no name in this image, so the
                 * name comes from the export table of the matching reference
                 * DLL (runtime/ordinal_names.c, generated from those tables).
                 * Without a name the slot keeps "#<ordinal>" and stays a named
                 * stop instead of being guessed. */
                const char *resolved = ordinal_name_lookup(dll, im->ordinal);
                if (resolved)
                    snprintf(im->name, sizeof im->name, "%s", resolved);
                else
                    snprintf(im->name, sizeof im->name, "#%u", im->ordinal);
            } else if (import_cstr(entry + 2, im->name, sizeof im->name)) {
                fprintf(stderr, "PORT_FATAL bad import name in %s\n", dll);
                return -1;
            }
            im->slot = IMAGE_BASE + ts + k * 4;
            im->thunk = import_entry_address(im->dll, im->name, g_import_count);
            g_import_count++;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* dynamic resolution (GetProcAddress) and the shim module table        */
/* ------------------------------------------------------------------ */

const char *static_module_name(uint32_t handle) {
    for (size_t i = 0; i < g_module_count; i++)
        if (g_modules[i].handle == handle) return g_modules[i].name;
    return NULL;
}

/* Resolve dll!name to a guest thunk address. A name that is a declared import
 * with a bound bridge reuses that thunk; anything else the runtime really
 * provides gets a fresh thunk in the same table (so RECOMP_ICALL reaches it
 * through recomp_lookup_import exactly like an IAT slot). Returns 0 when the
 * runtime does not provide the pair. */
uint32_t static_resolve_dynamic(const char *dll, const char *name) {
    for (uint32_t i = 0; i < g_import_count; i++) {
        if (!g_imports[i].bound) continue;
        if (strcmp(g_imports[i].dll, dll)) continue;
        if (strcmp(g_imports[i].name, name)) continue;
        return g_imports[i].thunk;
    }
    if (g_import_count >= MAX_IMPORTS) return 0;
    const StaticBridge *b = sb_find(dll, name);
    const GuestApiEntry *a4 = b ? NULL : guest_api_lookup(dll, name);
    if (a4 && !a4->invoke) a4 = NULL;
    if (!b && !a4) {
        /* Registered export the runtime has no body for: the guest still gets
         * its real registry address; calling it is the named stop. */
        uint32_t va = static_boot_loaded() ? static_boot_export_address(dll, name) : 0;
        if (!va) return 0;
        for (uint32_t i = 0; i < g_import_count; i++)
            if (g_imports[i].thunk == va) return va;
        StaticImport *un = &g_imports[g_import_count];
        memset(un, 0, sizeof *un);
        snprintf(un->dll, sizeof un->dll, "%s", dll);
        snprintf(un->name, sizeof un->name, "%s", name);
        un->thunk = va;
        g_import_count++;
        return va;
    }
    StaticImport *im = &g_imports[g_import_count];
    memset(im, 0, sizeof *im);
    snprintf(im->dll, sizeof im->dll, "%s", b ? sb_dll(b) : a4->dll);
    snprintf(im->name, sizeof im->name, "%s", b ? sb_name(b) : a4->name);
    im->slot = 0;
    im->thunk = import_entry_address(dll, name, g_import_count);
    im->argbytes = b ? sb_argbytes(b) : 0;
    im->bound = 1;
    im->bridge = b;
    im->a4 = a4;
    g_import_count++;
    return im->thunk;
}

/* ------------------------------------------------------------------ */
/* guest DLL image: the user's real dbdata.dll, run 1:1                 */
/* ------------------------------------------------------------------ */
/* <PORT_BOOT>/dbdata.bin is the DLL mapped at its preferred base, dbdata.idx
 * lists its IAT slots, ordinal exports and entry point (tools/dump_static_boot.py).
 * The module's own code is translated like the EXE's; its imports go to the
 * same bridges.  Same guest-visible protocol as the measurement driver
 * (runtime/proven_boot_main.c): import thunks from 0x50090000 in idx order,
 * DllMain(hinst, DLL_PROCESS_ATTACH, 0) on the caller's stack with the return
 * address DLLMAIN_SENTINEL, LoadLibraryA returns the image base.  Nothing in
 * the DLL is stubbed or patched. */
#define DLLMAIN_SENTINEL 0x5FFFFF00u
static struct {
    uint32_t base, size, entry;
    int mapped, attached;
    struct { uint32_t ord, va; } exp[16];
    int nexp;
} g_dbdata;
static RECOMP_TLS int g_bridge_keep_edx;
static RECOMP_TLS int g_bridge_abandon;   /* SEH: a bridge handed control to the guest (see static_raise_exception) */
void port_run_guest(uint32_t va, uint32_t sentinel);
int port_guest_dll_contains(uint32_t va) {
    return g_dbdata.mapped && va >= g_dbdata.base && va < g_dbdata.base + g_dbdata.size;
}
static int dbdata_map(void) {
    const char *dir = getenv("PORT_BOOT");
    if (!dir) return -1;
    char path[1024];
    snprintf(path, sizeof path, "%s/dbdata.idx", dir);
    FILE *f = fopen(path, "r");
    if (!f) return -2;
    uint32_t trap = 0x50090000u;
    for (uint32_t i = 0; i < g_import_count; i++)
        if (g_imports[i].thunk >= trap && g_imports[i].thunk < 0x50100000u) trap = g_imports[i].thunk + 16;
    char line[256], dll[48], name[80]; unsigned v, ord;
    struct { uint32_t slot, thunk; } binds[512]; int nb = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "P base %u", &v) == 1) g_dbdata.base = v;
        else if (sscanf(line, "P size %u", &v) == 1) g_dbdata.size = v;
        else if (sscanf(line, "P entry %u", &v) == 1) g_dbdata.entry = v;
        else if (line[0] == 'I' && sscanf(line, "I %x %47s %79s", &v, dll, name) == 3) {
            if (g_import_count >= MAX_IMPORTS || nb >= 512) { fclose(f); return -3; }
            const StaticBridge *b = sb_find(dll, name);
            const GuestApiEntry *a4 = b ? NULL : guest_api_lookup(dll, name);
            if (a4 && !a4->invoke) a4 = NULL;
            StaticImport *im = &g_imports[g_import_count++];
            memset(im, 0, sizeof *im);
            snprintf(im->dll, sizeof im->dll, "%s", dll);
            snprintf(im->name, sizeof im->name, "%s", name);
            im->slot = v;
            im->thunk = trap;
            im->argbytes = b ? sb_argbytes(b) : 0;
            im->bound = (b || a4) ? 1 : 0;      /* unbound: calling it is the named stop */
            im->bridge = b;
            im->a4 = a4;
            binds[nb].slot = v; binds[nb].thunk = trap; nb++;
            trap += 16;
        } else if (line[0] == 'E' && sscanf(line, "E %x %u", &v, &ord) == 2) {
            if (g_dbdata.nexp < 16) { g_dbdata.exp[g_dbdata.nexp].ord = ord; g_dbdata.exp[g_dbdata.nexp].va = v; g_dbdata.nexp++; }
        }
    }
    fclose(f);
    if (!g_dbdata.base || !g_dbdata.size || !g_dbdata.entry) return -4;
    snprintf(path, sizeof path, "%s/dbdata.bin", dir);
    f = fopen(path, "rb");
    if (!f) return -5;
    size_t got = fread(g_flat + g_dbdata.base, 1, g_dbdata.size, f);
    fclose(f);
    if (got != g_dbdata.size) return -6;
    for (int i = 0; i < nb; i++) memcpy(g_flat + binds[i].slot, &binds[i].thunk, 4);
    g_dbdata.mapped = 1;
    fprintf(stderr, "PORT_DBDATA mapped real dbdata.dll at 0x%08X (%u bytes), %d imports, %d exports, entry 0x%08X\n",
            g_dbdata.base, g_dbdata.size, nb, g_dbdata.nexp, g_dbdata.entry);
    return 0;
}
/* LoadLibraryA("dbdata.dll"): map once, run the real DllMain, return the base. */
static int dbdata_load(uint32_t *handle) {
    if (!g_dbdata.mapped) {
        int rc = dbdata_map();
        if (rc) { fprintf(stderr, "PORT_DBDATA map failed rc=%d (PORT_BOOT/dbdata.bin+idx)\n", rc); return GM_UNSUPPORTED; }
    }
    if (!g_dbdata.attached) {
        uint32_t s0 = g_esp, sp = s0, v;
        v = 0;                sp -= 4; memcpy(g_flat + sp, &v, 4);     /* lpReserved */
        v = 1;                sp -= 4; memcpy(g_flat + sp, &v, 4);     /* DLL_PROCESS_ATTACH */
        v = g_dbdata.base;    sp -= 4; memcpy(g_flat + sp, &v, 4);     /* hinstDLL */
        v = DLLMAIN_SENTINEL; sp -= 4; memcpy(g_flat + sp, &v, 4);
        g_esp = sp;
        port_run_guest(g_dbdata.entry, DLLMAIN_SENTINEL);
        fprintf(stderr, "PORT_DBDATA DllMain returned eax=0x%08X esp=0x%08X (expected esp 0x%08X)\n", g_eax, g_esp, s0);
        if (g_esp != s0) named_stop("DllMain-stack-imbalance", "dbdata.dll", "DllMain", g_esp);
        g_dbdata.attached = 1;
        g_bridge_keep_edx = 1;      /* caller-saved registers stay as DllMain left them */
    }
    *handle = g_dbdata.base;
    return GM_OK;
}

/* LoadLibraryA/W. A name the import directory already declared is served by
 * the shim loader (reference count + the same handle the IAT used). Any other
 * name -- the game delay-loads DSOUND, DINPUT8, XINPUT, ... -- is registered on
 * demand with a fresh opaque handle from the documented module pool, exactly
 * like Windows loading a DLL. No PE image is attached to such a module: every
 * GetProcAddress() on it resolves through the bridge table or stops, so the
 * registration is a *name*, never a fabricated export. */
static int static_load_library_inner(const char *name, uint32_t *handle);
/* Keep the guest-visible loader data (PEB_LDR_DATA module lists) in step with
 * the loader, exactly as the proven runtime does after LoadLibrary
 * (host_bridges.c run_loader): the protected code walks those lists itself. */
static int static_sync_loader_lists(void) {
    int rc = gp_sync_modules(g_mem, IMAGE_BASE, g_image_size, g_entry_va, &g_loader);
    if (rc) return rc;
    GuestModuleImage views[8];
    uint32_t n = static_boot_loaded() ? static_boot_view_count() : 0;
    for (uint32_t i = 0; i < n; i++) static_boot_view(i, &views[i].base, &views[i].size, &views[i].entry);
    return gp_sync_image_views(g_mem, &g_loader, views, n);
}
int static_load_library(const char *name, uint32_t *handle) {
    if (!strcasecmp(name, "dbdata.dll") && getenv("PORT_BOOT")) return dbdata_load(handle);
    int rc = static_load_library_inner(name, handle);
    if (rc || !g_peb) return rc;
    return static_sync_loader_lists();
}
static int static_load_library_inner(const char *name, uint32_t *handle) {
    if (!sl_load(&g_loader, name, handle)) return GM_OK;
    for (size_t i = 0; i < g_module_count; i++) {
        if (strcasecmp(g_modules[i].name, name)) continue;
        if (g_module_refs[i] == UINT32_MAX) return GM_UNSUPPORTED;
        g_module_refs[i]++;
        *handle = g_modules[i].handle;
        return GM_OK;
    }
    if (g_module_count >= 64) return GM_UNSUPPORTED;
    char *copy = strdup(name);
    if (!copy) return GM_ALLOCATION;
    size_t idx = g_module_count++;
    g_modules[idx].name = copy;
    /* With the boot registry the sized module pool is already occupied; put
     * on-demand names above it (below the process-parameter pages). */
    g_modules[idx].handle = (static_boot_loaded() ? 0x73000000u : MODULE_POOL_BASE) + (uint32_t)idx * 0x10000u;
    g_module_refs[idx] = 1;
    g_loader.module_count = g_module_count;
    *handle = g_modules[idx].handle;
    fprintf(stderr, "PORT_MODULE load %s handle=0x%08X (on demand)\n", name,
            g_modules[idx].handle);
    return GM_OK;
}
int static_free_library(uint32_t handle, uint32_t *result) {
    for (size_t i = 0; i < g_module_count; i++)
        if (g_modules[i].handle == handle) {
            if (!g_module_refs[i]) { *result = 0; return GM_OK; }
            g_module_refs[i]--;
            *result = 1;
            return GM_OK;
        }
    named_stop("FreeLibrary-unregistered", "kernel32.dll", "FreeLibrary", handle);
}

static int read_guest_ascii(uint32_t p, char *out, size_t cap) {
    size_t n = 0;
    for (; n + 1 < cap; n++) {
        uint8_t c = 0;
        if (gm_read(g_mem, p + (uint32_t)n, &c, 1)) return GM_UNCOMMITTED;
        if (!c) break;
        if (c >= 0x80) return GM_UNSUPPORTED;
        out[n] = (char)c;
    }
    out[n] = 0;
    return 0;
}

/* LoadLibraryA/W and GetProcAddress live here (not in static_bridges.c) so an
 * unresolvable name keeps its specific named stop. */
int static_load_library_a(const uint32_t *a, uint32_t *r) {
    char name[64];
    int rc = read_guest_ascii(a[0], name, sizeof name);
    if (rc) named_stop("LoadLibraryA-name", "kernel32.dll", "LoadLibraryA", a[0]);
    return static_load_library(name, r);
}
int static_load_library_w(const uint32_t *a, uint32_t *r) {
    char name[64];
    size_t n = 0;
    for (; n + 1 < sizeof name; n++) {
        uint16_t c = 0;
        if (gm_read(g_mem, a[0] + (uint32_t)n * 2, &c, 2)) break;
        if (!c) break;
        if (c > 0x7f) named_stop("LoadLibraryW-name", "kernel32.dll", "LoadLibraryW", a[0]);
        name[n] = (char)c;
    }
    name[n] = 0;
    return static_load_library(name, r);
}
/* LoadLibraryExW: the OS feature probe of the VCRuntime / OriginSDK
 * (LoadLibraryExW(name, 0, LOAD_LIBRARY_SEARCH_SYSTEM32) then GetProcAddress).
 * A module the registry knows returns its handle; anything else is the
 * documented "not present": NULL + ERROR_MOD_NOT_FOUND, which the callers fall
 * back on.  Same rule as the measurement driver; no reference count change. */
int static_load_library_ex_w(const uint32_t *a, uint32_t *r) {
    char base[64]; size_t n = 0;
    for (; n + 1 < sizeof base; n++) {
        uint16_t c = 0;
        if (gm_read(g_mem, a[0] + (uint32_t)n * 2, &c, 2) || !c) break;
        if (c >= 'A' && c <= 'Z') c += 32;
        base[n] = (char)c;
    }
    base[n] = 0;
    char *dot = strstr(base, ".dll"); if (dot) *dot = 0;
    uint32_t handle = 0;
    for (uint32_t i = 0; static_boot_loaded() && i < static_boot_module_count() && !handle; i++) {
        uint32_t h = 0; const char *mn = static_boot_module(i, &h);
        char mb[64]; size_t j = 0;
        for (const char *q = mn; *q && j + 1 < sizeof mb; q++) mb[j++] = (char)((*q >= 'A' && *q <= 'Z') ? *q + 32 : *q);
        mb[j] = 0;
        char *md = strstr(mb, ".dll"); if (md) *md = 0;
        if (!strcmp(mb, base)) handle = h;
    }
    if (!handle) { uint32_t le = 126; gm_write(g_mem, g_teb + 0x34, &le, 4); }
    fprintf(stderr, "PORT_LOADLIBEX %s -> 0x%08X%s\n", base, handle, handle ? "" : " (not provided; NULL/126)");
    *r = handle;
    return GM_OK;
}
int static_free_library_bridge(const uint32_t *a, uint32_t *r) {
    return static_free_library(a[0], r);
}
int static_get_proc_address(const uint32_t *a, uint32_t *r) {
    if (g_dbdata.mapped && a[0] == g_dbdata.base) {
        /* The DLL's own export: its real address in the mapped image. */
        if (a[1] >= 0x10000u) named_stop("GetProcAddress-dbdata-by-name", "dbdata.dll", "GetProcAddress", a[1]);
        for (int i = 0; i < g_dbdata.nexp; i++)
            if (g_dbdata.exp[i].ord == a[1]) {
                fprintf(stderr, "PORT_DBDATA GetProcAddress(dbdata, #%u) -> 0x%08X (real export)\n", a[1], g_dbdata.exp[i].va);
                *r = g_dbdata.exp[i].va;
                return GM_OK;
            }
        named_stop("GetProcAddress-dbdata-ordinal-unknown", "dbdata.dll", "#ordinal", a[1]);
    }
    const char *dll = static_module_name(a[0]);
    if (!dll) named_stop("GetProcAddress-unknown-module", "kernel32.dll",
                         "GetProcAddress", a[0]);
    char name[128];
    if (a[1] < 0x10000u) {
        /* A19: an ordinal export. The name is looked up in the ordinal table
         * generated from the reference DLLs' export tables; an ordinal that has
         * no name there stays a named stop (never a guess). */
        const char *resolved = ordinal_name_lookup(dll, a[1]);
        if (!resolved)
            named_stop("GetProcAddress-ordinal-unnamed", dll, "#ordinal", a[1]);
        snprintf(name, sizeof name, "%s", resolved);
        if (g_trace_exports)
            fprintf(stderr, "PORT_GPA_ORDINAL %s!%u -> %s\n", dll, a[1], name);
    } else if (read_guest_ascii(a[1], name, sizeof name)) {
        named_stop("GetProcAddress-name", dll, "GetProcAddress", a[1]);
    }
    uint32_t thunk = static_resolve_dynamic(dll, name);
    if (g_trace_exports)
        fprintf(stderr, "PORT_GPA %s!%s -> 0x%08X (lpProcName=0x%08X)\n", dll,
                name[0] ? name : "<ordinal>", thunk, a[1]);
    if (!thunk && orig_export_absent(dll, name)) {
        /* A deliberate platform decision (runtime/orig_bridges.c): this export
         * does not exist here, exactly as on a Windows version without it. */
        uint32_t le = 127; gm_write(g_mem, g_teb + 0x34, &le, 4);
        fprintf(stderr, "PORT_GPA_NULL %s!%s -> NULL/127 (not present on this platform)\n", dll, name);
        *r = 0;
        return GM_OK;
    }
    if (!thunk && g_resolve_unknown && g_import_count < MAX_IMPORTS) {
        /* Diagnostic mode (--resolve-unknown-exports): hand out a real thunk so
         * the stop happens where the code *calls* the export instead of where
         * it resolves it. The thunk has no bridge, so it still ends in
         * GUEST_API_UNIMPLEMENTED -- nothing is executed, nothing is faked. */
        StaticImport *im = &g_imports[g_import_count];
        memset(im, 0, sizeof *im);
        snprintf(im->dll, sizeof im->dll, "%s", dll);
        if (name[0]) snprintf(im->name, sizeof im->name, "%s", name);
        else snprintf(im->name, sizeof im->name, "#%u", a[1]);
        im->thunk = THUNK_BASE + g_import_count * THUNK_STRIDE;
        g_import_count++;
        thunk = im->thunk;
    }
    if (!thunk) {
        /* An export this runtime does not provide: the real API's answer for a
         * missing export is NULL + ERROR_PROC_NOT_FOUND, and the VCRuntime
         * feature probes (try_get_function) fall back on it.  Honest "not
         * present", never a fabricated address.  PORT_GPA_STRICT=1 keeps the
         * named stop for bring-up. */
        if (getenv("PORT_GPA_STRICT")) named_stop("GetProcAddress-unbridged", dll, name, a[1]);
        uint32_t le = 127; gm_write(g_mem, g_teb + 0x34, &le, 4);
        fprintf(stderr, "PORT_GPA_NULL %s!%s -> NULL/127 (not provided)\n", dll, name);
        *r = 0;
        return GM_OK;
    }
    *r = thunk;
    return GM_OK;
}

/* kernel32!ExitProcess: the port has no process-exit model (a guest thread
 * exits through ExitThread), so this is a named stop, never a silent return. */
int static_exit_process(const uint32_t *a, uint32_t *r) {
    (void)r;
    named_stop("ExitProcess", "kernel32.dll", "ExitProcess", a[0]);
}

/* ------------------------------------------------------------------ */
/* the import thunk                                                    */
/* ------------------------------------------------------------------ */

/* The single import thunk. recomp_lookup_import() selects the slot, then
 * RECOMP_ICALL calls this. It performs the Win32 call and the stdcall stack
 * cleanup, exactly as a native import thunk would. */
static void static_import_thunk(void) {
    int idx = g_pending_import;
    g_pending_import = -1;
    uint32_t esp = g_esp;
    /* Trampoline protocol: the thunk "returns" to the address on top of the
     * guest stack, whoever put it there (a call site or the VM's own push). */
    uint32_t ret_va = 0;
    if (gm_read(g_mem, esp, &ret_va, 4)) named_stop("import-return-address", "?", "?", esp);
    g_next = ret_va;
    static int trace_api = -1;
    static uint64_t api_calls;
    if (trace_api < 0) trace_api = getenv("PORT_TRACE_API") ? atoi(getenv("PORT_TRACE_API")) : 0;
    api_calls++;
    if (trace_api && idx >= 0 && (uint32_t)idx < g_import_count) {
        uint32_t a[4] = {0, 0, 0, 0};
        gm_read(g_mem, esp + 4, a, sizeof a);
        fprintf(stderr, "PORT_API #%llu %s!%s ret=0x%08X args=%08X %08X %08X %08X from=0x%08X\n",
                (unsigned long long)api_calls, g_imports[idx].dll, g_imports[idx].name,
                ret_va, a[0], a[1], a[2], a[3], g_cur_func);
        if (trace_api >= 3)                         /* string preview of pointer arguments */
            for (int k = 0; k < 2; k++) {
                uint32_t p = a[k];
                if (p < 0x10000u || p >= 0xFFF00000u) continue;
                const uint8_t *t = g_flat + p; int n = 0;
                while (n < 96 && t[n] >= 0x20 && t[n] < 0x7F) n++;
                if (n >= 3 && t[n] == 0) fprintf(stderr, "PORT_API_STR   arg%d=\"%.*s\"\n", k, n, (const char *)t);
            }
    }
    if (idx < 0 || (uint32_t)idx >= g_import_count)
        named_stop("import-index", "?", "?", 0);
    StaticImport *im = &g_imports[idx];
    if (!im->bridge) {
        /* Not in the merged bridge table: hand over to the A4 manifest row.
         * Its `run_*` adapter (runtime/host_bridges.c) pops the stdcall frame
         * itself and writes the real CPU globals through the hb accessors, so
         * the snapshot it returns is applied here as the authoritative state.
         * A row without an adapter is still a named stop. */
        const GuestApiEntry *e = im->a4 ? im->a4 : guest_api_lookup(im->dll, im->name);
        if (e && e->invoke) {
            uint32_t snapshot[16];
            memset(snapshot, 0, sizeof snapshot);
            int arc = e->invoke(e->which, snapshot);
            if (arc) named_stop("api-adapter-unsupported", im->dll, im->name, im->slot);
            g_eax = snapshot[0]; g_ecx = snapshot[1]; g_edx = snapshot[2];
            g_ebx = snapshot[3]; g_esp = snapshot[4]; g_ebp = snapshot[5];
            g_esi = snapshot[6]; g_edi = snapshot[7];
            g_next = ret_va;        /* a bridge may have run guest code meanwhile */
            if (trace_api >= 2)
                fprintf(stderr, "PORT_API_RET #%llu %s!%s -> eax=0x%08X (a4)\n", (unsigned long long)api_calls, im->dll, im->name, g_eax);
            return;
        }
        named_stop("GUEST_API_UNIMPLEMENTED", im->dll, im->name, im->slot);
    }
    uint32_t eax = 0, edx = g_edx;
    int st0_valid = 0;
    uint64_t st0_bits = 0;
    int rc = sb_invoke(im->bridge, esp, &eax, &edx, &st0_valid, &st0_bits);
#ifndef PORT_PCPU
    if (!rc && g_bridge_abandon) { g_bridge_abandon = 0; return; }   /* SEH: the guest took control */
#endif
    if (rc == 0x101) {                              /* GTA_STOP_THREAD_EXIT: ExitThread */
        st_thread_exit_current();                   /* does not return on a guest thread */
        named_stop("ExitThread-on-main-thread", im->dll, im->name, MEM32(esp + 4));
    }
    if (rc) {
        fprintf(stderr, "PORT_BRIDGE_RC %s!%s rc=%d args=%08X %08X %08X %08X\n", im->dll, im->name, rc,
                MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), MEM32(esp + 16));
        named_stop("bridge-unsupported", im->dll, im->name, im->slot);
    }
    g_eax = eax;
    if (g_bridge_keep_edx) g_bridge_keep_edx = 0;   /* a bridge that ran guest code (DllMain) */
    else g_edx = edx;
    g_esp = esp + 4 + (sb_cdecl(im->bridge) ? 0 : im->argbytes);
    g_next = ret_va;                /* a bridge may have run guest code meanwhile */
    /* An API call is where a uniprocessor reschedules: if this call (SetEvent,
     * ReleaseSemaphore, LeaveCriticalSection ...) or anything else made a
     * waiting thread ready, it runs now. The CPU state is complete here. */
    if (st_machine_preempt_pending()) st_machine_yield();
    if (trace_api >= 2)
        fprintf(stderr, "PORT_API_RET #%llu %s!%s -> eax=0x%08X\n", (unsigned long long)api_calls, im->dll, im->name, g_eax);
}

/* ------------------------------------------------------------------ */
/* dispatch: recomp_lookup / recomp_lookup_import                      */
/* ------------------------------------------------------------------ */

static void unlifted_target(uint32_t va) __attribute__((noreturn));
static void unlifted_target(uint32_t va) {
    const char *sec = section_of(va);
    if (sec && strcmp(sec, ".trace") == 0)
        named_stop("trace-target", NULL, NULL, va);
    named_stop("unlifted-target", NULL, NULL, va);
}

static void lookup_trap_thunk(void) {
    /* recomp_lookup() stores the VA it could not resolve before returning this
     * function, so the trap can name it. */
    unlifted_target(g_last_eip);
}

/* Code the image writes at run time (runtime/static_overlay.c, generated by
 * tools/static_overlay_lift.py). The overlay entry checks the live guest bytes
 * itself and stops by name on a state the witness never showed. */
recomp_func_t static_overlay_lookup(uint32_t va);
recomp_func_t recomp_lookup_manual(uint32_t va) {
    return static_overlay_lookup(va);
}

/* Hot-discovery loader (runtime/static_hot.c); inert unless PORT_HOT=1. */
recomp_func_t port_hot_resolve(uint32_t va);

/* Trampoline protocol (runtime/recomp_override/recomp_types.h): run the
 * function at g_next once. It leaves the following target in g_next. */
/* g_dispatch_count: member of g_cpu (recomp_types.h). */
#ifdef PORT_PCPU
static void lookup_trap_thunk(void);
/* PF dispatch: c->eip names the next function.  Generated PF functions work on
 * the CPU directly; the import thunk (and only it) runs on the g_e* globals. */
void port_dispatch_next(void) {
    uint32_t va = pf_eip();
    static int dtrace = -1, hot_all = -1;
    if (dtrace < 0) { const char *e = getenv("PORT_DTRACE"); dtrace = (e && e[0] == '1') ? 1 : 0; }
    if (hot_all < 0) { const char *e = getenv("PORT_HOT_ALL"); hot_all = (e && e[0] == '1') ? 1 : 0; }
    if (dtrace) pf_dispatch_trace((unsigned long long)(g_dispatch_count + 1));
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = va;
    g_icall_from[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = g_cur_func;
    g_icall_trace_idx++; g_icall_count++;
    recomp_func_t fn = recomp_lookup_manual(va);
    if (!fn && hot_all) fn = port_hot_resolve(va);
    if (!fn && (va < 0x10000000u || port_guest_dll_contains(va))) {
        fn = recomp_lookup(va);                 /* table, then hot-discovery */
        if (fn == lookup_trap_thunk) fn = NULL;
    }
    g_dispatch_count++;
    if (fn) { g_cur_func = va; fn(); return; }
    fn = recomp_lookup_import(va);
    if (!fn) named_stop("unlifted-target", NULL, NULL, va);
    pf_to_globals(); g_pf_live = 0;
    g_next = va;
    fn();
    pf_from_globals(); pf_set_eip(g_next); g_pf_live = 1;
}
/* Run guest code from `va` until control reaches `sentinel`.  Entered with the
 * g_e* globals authoritative (process entry, thread start, bridge callback)
 * and left the same way. */
void port_run_guest(uint32_t va, uint32_t sentinel) {
    pf_from_globals(); pf_set_eip(va); g_pf_live = 1;
    while (pf_eip() != sentinel) port_dispatch_next();
    pf_to_globals(); g_pf_live = 0;
    g_next = sentinel;
}
#else
/* Dispatch trace (dev only, PORT_DTRACE=1): one line per dispatched function
 * with the incoming register state, so it can be diffed instruction-for-value
 * against the precise oracle's PROVEN_STEP_TRACE to locate the first divergence. */
/* Indirect transfers (virtual calls, function pointers, jump tables that leave
 * a body) come through here millions of times per second. A direct-mapped
 * cache in front of the binary search over the 35,000-entry table: one word
 * per slot, (va << 32) | (table index + 1), read and written atomically so it
 * needs no lock. Only static-table results are cached (they never change). */
#ifdef RECOMP_CPU_REG_ACTIVE
/* Entering guest code: lifted functions find the CPU state through x28. */
#define RECOMP_CPU_ENTER() recomp_cpu_t *_cpu_saved = recomp_cpu_reg; recomp_cpu_reg = &g_cpu_tls
#define RECOMP_CPU_LEAVE() recomp_cpu_reg = _cpu_saved
#define RECOMP_CPU_HERE() recomp_cpu_reg          /* valid whenever guest code is running */
#else
#define RECOMP_CPU_ENTER() ((void)0)
#define RECOMP_CPU_LEAVE() ((void)0)
#define RECOMP_CPU_HERE() (&g_cpu_tls)
#endif
static void static_d3d9_thunk(void);
static void static_import_thunk(void);
#define DISPATCH_CACHE_BITS 16
static _Atomic uint64_t g_dispatch_cache[1u << DISPATCH_CACHE_BITS];
static int g_dispatch_plain = -1;   /* 1 = no dev tracing/overrides: the cache may be used */
static uint32_t dispatch_table_find(uint32_t va) {      /* index + 1, 0 = not in the table */
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (recomp_dispatch_table[mid].address < va) lo = mid + 1;
        else hi = mid;
    }
    return (lo < recomp_dispatch_count && recomp_dispatch_table[lo].address == va) ? lo + 1 : 0;
}
void port_dispatch_next(void) {
    recomp_cpu_t *cpu = RECOMP_CPU_HERE();
    uint32_t va = cpu->c_next;
    uint32_t slot = (va * 0x9E3779B1u) >> (32 - DISPATCH_CACHE_BITS);
    uint64_t cached = atomic_load_explicit(&g_dispatch_cache[slot], memory_order_relaxed);
    if ((uint32_t)(cached >> 32) == va && (uint32_t)cached) {
        cpu->c_dispatch_count++;
        recomp_dispatch_table[(uint32_t)cached - 1].func();
        return;
    }
    /* The two other frequent kinds: a Direct3D method (thousands per frame)
     * and an API import thunk. Same result as the general path below. */
    if (d3d9t_is_trap(va)) {
        cpu->c_dispatch_count++;
        cpu->c_pending_trap = va;
        static_d3d9_thunk();
        return;
    }
    if (va >= THUNK_BASE && va < THUNK_BASE + MAX_IMPORTS * THUNK_STRIDE && g_dispatch_plain > 0) {
        uint32_t i = (va - THUNK_BASE) / THUNK_STRIDE;
        if (i < g_import_count && THUNK_BASE + i * THUNK_STRIDE == va && g_imports[i].thunk == va) {
            cpu->c_dispatch_count++;
            cpu->c_pending_import = (int)i;
            static_import_thunk();
            return;
        }
    }
    if (g_dispatch_plain < 0) {
        const char *a = getenv("PORT_DTRACE"), *b = getenv("PORT_HOT_ALL");
        g_dispatch_plain = !((a && a[0] == '1') || (b && b[0] == '1'));
    }
    if (g_dispatch_plain && va < 0x10000000u && !recomp_lookup_manual(va) && !d3d9t_is_trap(va)) {
        uint32_t found = dispatch_table_find(va);
        if (found) {
            atomic_store_explicit(&g_dispatch_cache[slot], ((uint64_t)va << 32) | found, memory_order_relaxed);
            g_dispatch_count++;
            recomp_dispatch_table[found - 1].func();
            return;
        }
    }
    static int dtrace = -1;
    if (dtrace < 0) { const char *e = getenv("PORT_DTRACE"); dtrace = (e && e[0] == '1') ? 1 : 0; }
    if (dtrace)
        fprintf(stderr, "FT %llu next=%08X eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
                (unsigned long long)(g_dispatch_count + 1), va, g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    recomp_func_t fn = recomp_lookup_manual(va);
    /* PORT_HOT_ALL=1 (dev): every image function comes freshly lifted from the
     * helper (current lifter), bypassing the static table -- the fast loop for
     * bisecting lifter bugs against the oracle without a full re-lift. */
    static int hot_all = -1;
    if (hot_all < 0) { const char *e = getenv("PORT_HOT_ALL"); hot_all = (e && e[0] == '1') ? 1 : 0; }
    if (!fn && hot_all) fn = port_hot_resolve(va);
    if (!fn && va >= 0x10000000u) fn = recomp_lookup_import(va);   /* outside the image */
    if (!fn) fn = recomp_lookup(va);
    if (!fn) fn = recomp_lookup_import(va);
    if (!fn) named_stop("unlifted-target", NULL, NULL, va);
    g_dispatch_count++;
    fn();
}
/* Run guest code from `va` until control reaches `sentinel` (the return
 * address the caller pushed). */
void port_run_guest(uint32_t va, uint32_t sentinel) {
    RECOMP_CPU_ENTER();
    g_next = va;
    while (g_next != sentinel) port_dispatch_next();
    RECOMP_CPU_LEAVE();
}

#endif

/* ------------------------------------------------------------------ */
/* structured exception handling (x86 SEH, fs:[0] chain)                */
/* ------------------------------------------------------------------ */
/* RaiseException / RtlUnwind the way ntdll does them: walk the guest's own
 * EXCEPTION_REGISTRATION chain and call the guest's own handlers
 * (_except_handler4, __CxxFrameHandler3 ... -- all translated game code).
 * Nothing about the exception is decided here.
 *
 * A handler that "executes the handler" never returns: it unwinds and jumps
 * into the __except/catch block of some outer function.  In the trampoline
 * protocol that is a frame abandonment: port_run_guest_until() reports it when
 * the guest has left the nested call's stack level, the bridge sets
 * g_bridge_abandon, and the import thunk leaves the CPU state exactly as the
 * guest set it so every C frame above unwinds to the dispatcher. */
#ifndef PORT_PCPU
#define SEH_SENTINEL 0x5FFFFE00u
/* Run guest code from `va`. 0 = it returned to `sentinel`; 1 = control left the
 * nested level (esp above `esp_limit`) without returning: abandoned. */
static int port_run_guest_until(uint32_t va, uint32_t sentinel, uint32_t esp_limit) {
    RECOMP_CPU_ENTER();
    int left = 0;
    g_next = va;
    for (;;) {
        if (g_next == sentinel) break;
        if (g_esp > esp_limit) { left = 1; break; }
        port_dispatch_next();
    }
    RECOMP_CPU_LEAVE();
    return left;
}
static void seh_build_context(uint32_t ctx, uint32_t eip, uint32_t esp) {
    memset(g_flat + ctx, 0, 0x2CC);
    uint32_t *c = (uint32_t *)(g_flat + ctx);
    c[0x00 / 4] = 0x00010007u;                      /* CONTEXT_FULL */
    c[0x8C / 4] = 0x2B; c[0x90 / 4] = 0x53; c[0x94 / 4] = 0x2B; c[0x98 / 4] = 0x2B;   /* gs fs es ds */
    c[0x9C / 4] = g_edi; c[0xA0 / 4] = g_esi; c[0xA4 / 4] = g_ebx;
    c[0xA8 / 4] = g_edx; c[0xAC / 4] = g_ecx; c[0xB0 / 4] = g_eax;
    c[0xB4 / 4] = g_ebp; c[0xB8 / 4] = eip; c[0xBC / 4] = 0x23;
    c[0xC0 / 4] = recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1);
    c[0xC4 / 4] = esp; c[0xC8 / 4] = 0x2B;
}
/* handler(record, frame, context, dispatcher_context), cdecl. Returns 1 when abandoned. */
static int seh_call_handler(uint32_t handler, uint32_t rec, uint32_t frame, uint32_t ctx,
                            uint32_t dc, uint32_t sp, uint32_t *disposition) {
    sp -= 4; memcpy(g_flat + sp, &dc, 4);
    sp -= 4; memcpy(g_flat + sp, &ctx, 4);
    sp -= 4; memcpy(g_flat + sp, &frame, 4);
    sp -= 4; memcpy(g_flat + sp, &rec, 4);
    uint32_t sentinel = SEH_SENTINEL;
    sp -= 4; memcpy(g_flat + sp, &sentinel, 4);
    g_esp = sp;
    if (port_run_guest_until(handler, SEH_SENTINEL, sp + 4)) return 1;
    *disposition = g_eax;
    return 0;
}
/* Call a guest stdcall function from inside a bridge (window procedures,
 * enumeration callbacks, thread-less timers).  The frame is built below the
 * API's own stack frame; the callee pops its arguments.  Returns 1 when the
 * guest left through an exception instead of returning (the caller must then
 * return GM_OK immediately: g_bridge_abandon is set). */
#define CALLBACK_SENTINEL 0x5FFFFD00u
int port_guest_stdcall(uint32_t va, const uint32_t *args, uint32_t count, uint32_t *result) {
    uint32_t saved_esp = g_esp, saved_next = g_next;
    uint32_t sp = (g_esp - 0x80u) & ~3u;
    for (uint32_t i = count; i-- > 0;) { sp -= 4; memcpy(g_flat + sp, &args[i], 4); }
    uint32_t sentinel = CALLBACK_SENTINEL;
    sp -= 4; memcpy(g_flat + sp, &sentinel, 4);
    g_esp = sp;
    if (port_run_guest_until(va, CALLBACK_SENTINEL, sp + 4 + count * 4)) { g_bridge_abandon = 1; return 1; }
    if (result) *result = g_eax;
    g_esp = saved_esp; g_next = saved_next;
    return 0;
}
static int user32_guest_call(void *context, uint32_t address, const uint32_t *args, uint32_t count, uint32_t *result) {
    (void)context;
    return port_guest_stdcall(address, args, count, result) ? GM_UNSUPPORTED : 0;
}

int static_raise_exception(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint32_t esp0 = g_esp, ret_va = 0;
    memcpy(&ret_va, g_flat + esp0, 4);
    uint32_t rec = (esp0 - 0x60u) & ~3u, ctx = rec - 0x2D0u, dc = ctx - 16u, sp = dc - 16u;
    uint32_t nargs = a[2] > 15u ? 15u : a[2];
    memset(g_flat + rec, 0, 0x50);
    uint32_t *e = (uint32_t *)(g_flat + rec);
    e[0] = a[0]; e[1] = a[1] & 1u;                  /* only EXCEPTION_NONCONTINUABLE is caller-settable */
    e[3] = ret_va; e[4] = a[3] ? nargs : 0;
    if (a[3]) memcpy(&e[5], g_flat + a[3], nargs * 4u);
    seh_build_context(ctx, ret_va, esp0 + 20u);
    memset(g_flat + dc, 0, 16);
    static int trace = -1;
    if (trace < 0) trace = getenv("PORT_TRACE_SEH") ? 1 : 0;
    if (trace) fprintf(stderr, "PORT_SEH raise code=0x%08X flags=0x%X nargs=%u from=0x%08X\n", a[0], a[1], a[2], ret_va);
    uint32_t frame = 0;
    memcpy(&frame, g_flat + g_teb, 4);
    while (frame != 0xFFFFFFFFu) {
        uint32_t next = 0, handler = 0, disposition = 0;
        memcpy(&next, g_flat + frame, 4);
        memcpy(&handler, g_flat + frame + 4, 4);
        if (trace) fprintf(stderr, "PORT_SEH   frame=0x%08X handler=0x%08X\n", frame, handler);
        if (seh_call_handler(handler, rec, frame, ctx, dc, sp, &disposition)) {
            if (trace) fprintf(stderr, "PORT_SEH   handler 0x%08X took the exception (continues at 0x%08X esp=0x%08X)\n", handler, g_next, g_esp);
            g_bridge_abandon = 1;
            return GM_OK;
        }
        if (disposition == 0) {                     /* ExceptionContinueExecution */
            if (e[1] & 1u) named_stop("noncontinuable-exception-continued", "kernel32.dll", "RaiseException", a[0]);
            const uint32_t *c = (const uint32_t *)(g_flat + ctx);
            g_edi = c[0x9C / 4]; g_esi = c[0xA0 / 4]; g_ebx = c[0xA4 / 4];
            g_edx = c[0xA8 / 4]; g_ecx = c[0xAC / 4]; g_eax = c[0xB0 / 4];
            g_ebp = c[0xB4 / 4]; g_next = c[0xB8 / 4]; g_esp = c[0xC4 / 4];
            g_flag_k = FK_EFLAGS; g_flag_a = c[0xC0 / 4]; g_flag_b = 0; g_flag_cf = c[0xC0 / 4] & 1u;
            g_bridge_abandon = 1;
            return GM_OK;
        }
        if (disposition != 1) named_stop("seh-disposition-unsupported", "kernel32.dll", "RaiseException", disposition);
        frame = next;
    }
    /* No handler took it: the process would die in UnhandledExceptionFilter. */
    named_stop("unhandled-exception", "kernel32.dll", "RaiseException", a[0]);
}
int static_rtl_unwind(const uint32_t *a, uint32_t *r) {
    uint32_t esp0 = g_esp, target = a[0], rec = a[2];
    uint32_t ctx = (esp0 - 0x2D0u - 0x60u) & ~3u, dc = ctx - 16u, sp = dc - 16u;
    if (!rec) {                                     /* RtlUnwind makes its own STATUS_UNWIND record */
        rec = (esp0 - 0x58u) & ~3u;
        memset(g_flat + rec, 0, 0x50);
        uint32_t code = 0xC0000027u; memcpy(g_flat + rec, &code, 4);
    }
    uint32_t flags = 0;
    memcpy(&flags, g_flat + rec + 4, 4);
    flags |= target ? 2u : 6u;                      /* EXCEPTION_UNWINDING (| EXIT_UNWIND) */
    memcpy(g_flat + rec + 4, &flags, 4);
    uint32_t ret_va = 0; memcpy(&ret_va, g_flat + esp0, 4);
    seh_build_context(ctx, ret_va, esp0 + 20u);
    memset(g_flat + dc, 0, 16);
    /* callee-saved registers survive the handler calls (they are cdecl functions) */
    uint32_t frame = 0;
    memcpy(&frame, g_flat + g_teb, 4);
    while (frame != 0xFFFFFFFFu && frame != target) {
        if (target && frame > target) named_stop("seh-invalid-unwind-target", "kernel32.dll", "RtlUnwind", target);
        uint32_t next = 0, handler = 0, disposition = 0;
        memcpy(&next, g_flat + frame, 4);
        memcpy(&handler, g_flat + frame + 4, 4);
        if (seh_call_handler(handler, rec, frame, ctx, dc, sp, &disposition)) { g_bridge_abandon = 1; return GM_OK; }
        if (disposition != 1) named_stop("seh-unwind-disposition-unsupported", "kernel32.dll", "RtlUnwind", disposition);
        memcpy(g_flat + g_teb, &next, 4);           /* unlink the unwound frame */
        frame = next;
    }
    g_esp = esp0;                                   /* the thunk pops the stdcall frame */
    *r = a[3];
    return GM_OK;
}
#else
int static_raise_exception(const uint32_t *a, uint32_t *r) { (void)r; named_stop("seh-not-in-pf-engine", "kernel32.dll", "RaiseException", a[0]); }
int static_rtl_unwind(const uint32_t *a, uint32_t *r) { (void)r; named_stop("seh-not-in-pf-engine", "kernel32.dll", "RtlUnwind", a[0]); }
#endif

/* Diagnostics for a run that neither stops nor advances: SIGUSR1 makes every
 * guest thread print where it is (current function, stack pointer, the last
 * indirect transfers and the return addresses found on its stack). Read-only;
 * async-signal-unsafe stdio is accepted for this development aid. */
#include <signal.h>
void st_signal_others(int sig);
static void diag_dump(int sig) {
    char line[1600]; int n = 0;
    n += snprintf(line + n, sizeof line - n, "PORT_DIAG teb=0x%08X func=0x%08X next=0x%08X esp=0x%08X eax=%08X ecx=%08X edx=%08X icalls=%u chain:",
                  g_teb, g_cur_func, g_next, g_esp, g_eax, g_ecx, g_edx, (unsigned)g_icall_count);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t k = (g_icall_trace_idx - 1 - i) & (ICALL_TRACE_SIZE - 1);
        n += snprintf(line + n, sizeof line - n, " %08X<-%08X", g_icall_trace[k], g_icall_from[k]);
    }
    n += snprintf(line + n, sizeof line - n, " stack:");
    uint32_t text_lo = IMAGE_BASE + 0x1000u, text_hi = IMAGE_BASE + 0x62F000u, found = 0;
    for (uint32_t off = 0; off < 0x2000u && found < 20 && n < (int)sizeof line - 12; off += 4) {
        uint32_t v; memcpy(&v, g_flat + g_esp + off, 4);
        if (v >= text_lo && v < text_hi) { n += snprintf(line + n, sizeof line - n, " %08X", v); found++; }
    }
    line[n++] = '\n';
    if (write(2, line, (size_t)n) < 0) { }
    if (sig == SIGUSR1) st_signal_others(SIGUSR2);   /* every other running guest thread, and the main thread */
}
static void diag_install(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = diag_dump; sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL); sigaction(SIGUSR2, &sa, NULL);
}

/* Debugger hook (port_debug_hit): PORT_FIND_HEX, PORT_WATCH_ONLINE_UI. */
__attribute__((noinline)) void port_debug_hit(uint32_t guest, volatile void *host) {
    fprintf(stderr, "PORT_FIND hit at guest 0x%08X (host %p)\n", guest, (void *)host);
}

#ifndef __ANDROID__
/* Host only: scripted input for unattended runs.
 *   PORT_INPUT_SCRIPT="<seconds>:pad:<A+RT+LX-...>:<hold seconds>,<seconds>:key:<VK name>:<hold seconds>,..."
 * Events go through the same entry points the Android front end uses
 * (port_set_gamepad_state / port_post_host_input). Times count from start. */
int port_post_host_input(uint32_t type, uint32_t x, uint32_t y, uint32_t code);
static double script_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void script_pad(const char *names, int down) {
    port_gamepad_state st; memset(&st, 0, sizeof st);
    st.abi_version = PORT_HOST_API_ABI; st.connected = 1; st.axis_count = PORT_AXIS_COUNT;
    char copy[128]; snprintf(copy, sizeof copy, "%s", names);
    for (char *t = strtok(copy, "+"); down && t; t = strtok(NULL, "+")) {
        static const struct { const char *n; int bit; } buttons[] = { {"START", PORT_PAD_START}, {"BACK", PORT_PAD_BACK},
            {"L3", PORT_PAD_LEFT_THUMB}, {"R3", PORT_PAD_RIGHT_THUMB}, {"LB", PORT_PAD_LEFT_SHOULDER}, {"RB", PORT_PAD_RIGHT_SHOULDER},
            {"A", PORT_PAD_A}, {"B", PORT_PAD_B}, {"X", PORT_PAD_X}, {"Y", PORT_PAD_Y} };
        static const struct { const char *n; int bit; } dpad[] = { {"UP", PORT_PAD_DPAD_UP}, {"DOWN", PORT_PAD_DPAD_DOWN},
            {"LEFT", PORT_PAD_DPAD_LEFT}, {"RIGHT", PORT_PAD_DPAD_RIGHT} };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++) if (!strcmp(t, buttons[i].n)) st.buttons |= 1u << buttons[i].bit;
        for (size_t i = 0; i < sizeof dpad / sizeof dpad[0]; i++) if (!strcmp(t, dpad[i].n)) st.dpad |= 1u << dpad[i].bit;
        if (!strcmp(t, "RT")) st.axis[PORT_AXIS_RIGHT_TRIGGER] = 1.0f;
        if (!strcmp(t, "LT")) st.axis[PORT_AXIS_LEFT_TRIGGER] = 1.0f;
        if (!strcmp(t, "LX-")) st.axis[PORT_AXIS_LEFT_X] = -1.0f;
        if (!strcmp(t, "LX+")) st.axis[PORT_AXIS_LEFT_X] = 1.0f;
        if (!strcmp(t, "LY-")) st.axis[PORT_AXIS_LEFT_Y] = -1.0f;
        if (!strcmp(t, "LY+")) st.axis[PORT_AXIS_LEFT_Y] = 1.0f;
    }
    port_set_gamepad_state(&st);
}
static void *input_script_thread(void *arg) {
    pthread_setname_np(pthread_self(), "port-input");
    char *script = (char *)arg, *save = NULL;
    double start = script_now();
    for (char *event = strtok_r(script, ",", &save); event; event = strtok_r(NULL, ",", &save)) {
        double at = 0, hold = 0.2; char kind[8] = "", what[96] = "";
        if (sscanf(event, "%lf:%7[^:]:%95[^:]:%lf", &at, kind, what, &hold) < 3) { fprintf(stderr, "PORT_INPUT bad script event '%s'\n", event); continue; }
        while (script_now() - start < at) usleep(20000);
        fprintf(stderr, "PORT_INPUT t=%.1fs %s %s for %.2fs\n", script_now() - start, kind, what, hold);
        if (!strcmp(kind, "pad")) { script_pad(what, 1); usleep((useconds_t)(hold * 1e6)); script_pad(what, 0); }
        else if (!strcmp(kind, "key")) {
            uint32_t vk = ain_vk_from_name(what);
            if (!vk) { fprintf(stderr, "PORT_INPUT unknown key '%s'\n", what); continue; }
            port_post_host_input(GU_INPUT_KEY_DOWN, 0, 0, vk); usleep((useconds_t)(hold * 1e6)); port_post_host_input(GU_INPUT_KEY_UP, 0, 0, vk);
        }
    }
    return NULL;
}
/* Host only: live input for interactive debugging runs.
 *   PORT_INPUT_FIFO=<path>   a named pipe; each line "pad:<names>:<hold s>" or
 *                            "key:<VK name>:<hold s>" is applied when it arrives
 *                            (same entry points as the script above). */
static void *input_fifo_thread(void *arg) {
    pthread_setname_np(pthread_self(), "port-input-fifo");
    const char *path = (const char *)arg;
    for (;;) {
        FILE *f = fopen(path, "r");                  /* blocks until a writer opens it */
        if (!f) { usleep(200000); continue; }
        char line[160];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            double hold = 0.2; char kind[8] = "", what[96] = "";
            if (sscanf(line, "%7[^:]:%95[^:]:%lf", kind, what, &hold) < 2) { fprintf(stderr, "PORT_INPUT bad fifo line '%s'\n", line); continue; }
            fprintf(stderr, "PORT_INPUT fifo %s %s for %.2fs\n", kind, what, hold);
            if (!strcmp(kind, "pad")) { script_pad(what, 1); usleep((useconds_t)(hold * 1e6)); script_pad(what, 0); }
            else if (!strcmp(kind, "key")) {
                uint32_t vk = ain_vk_from_name(what);
                if (!vk) { fprintf(stderr, "PORT_INPUT unknown key '%s'\n", what); continue; }
                port_post_host_input(GU_INPUT_KEY_DOWN, 0, 0, vk); usleep((useconds_t)(hold * 1e6)); port_post_host_input(GU_INPUT_KEY_UP, 0, 0, vk);
            }
        }
        fclose(f);
    }
    return NULL;
}
/* Host only, debugging: PORT_FIND_HEX=<bytes> [PORT_FIND_AFTER=<s>] scans the
 * guest heap range after that many seconds and reports every match through
 * port_debug_hit() (a place for a debugger breakpoint, e.g. to set a read
 * watchpoint on game data). */
static void *find_thread(void *arg) {
    (void)arg;
    const char *hex = getenv("PORT_FIND_HEX"), *after = getenv("PORT_FIND_AFTER");
    uint8_t pat[64]; size_t n = 0;
    for (const char *h = hex; h[0] && h[1] && n < sizeof pat; h += 2) {
        unsigned v; if (sscanf(h, "%2x", &v) != 1) break; pat[n++] = (uint8_t)v;
    }
    sleep(after ? (unsigned)atoi(after) : 20);
    if (!n || !g_flat) return NULL;
    uint32_t hits[16]; unsigned nhits = 0;
    for (uint32_t va = 0x01000000u; va < 0x60000000u - n && nhits < 16; ) {
        uint8_t *p = memmem(g_flat + va, (size_t)(0x60000000u - va), pat, n);
        if (!p) break;
        hits[nhits++] = (uint32_t)(p - g_flat);
        va = (uint32_t)(p - g_flat) + 1;
    }
    for (unsigned i = 0; i < nhits; i++) port_debug_hit(hits[i], g_flat + hits[i]);   /* after the scan */
    fprintf(stderr, "PORT_FIND done\n");
    return NULL;
}
static void input_script_start(void) {
    if (getenv("PORT_FIND_HEX")) { pthread_t t; pthread_create(&t, NULL, find_thread, NULL); pthread_detach(t); }
    const char *fifo = getenv("PORT_INPUT_FIFO");
    if (fifo && *fifo) {
        pthread_t thread;
        pthread_create(&thread, NULL, input_fifo_thread, strdup(fifo));
        pthread_detach(thread);
    }
    const char *script = getenv("PORT_INPUT_SCRIPT");
    if (!script || !*script) return;
    if (strstr(script, ":pad:")) script_pad("", 0);         /* a pad is connected from the start */
    pthread_t thread;
    pthread_create(&thread, NULL, input_script_thread, strdup(script));
    pthread_detach(thread);
}
#endif

/* Port behaviour patch switch (tools/port_patches.py). */
int port_skip_online_login = 1;
/* Arguments of the result report (0x005E9EC0) as bytes a0 | a1 << 8 | a2 << 16; PORT_SKIP_ONLINE_ARGS (hex). */
int port_skip_online_args = 0;
/* Keep the online module's current state instead of taking over the requested one (PORT_SKIP_ONLINE_KEEP). */
int port_skip_online_keep = 0;
/* Port patch skip-online-error: no "Connection Problem" dialog (PORT_ONLINE_ERROR=1 shows it). */
int port_skip_online_error = 1;
/* Frames after the dialog opened when the port presses OK (PORT_AUTO_CONFIRM_FRAMES). */
int port_auto_confirm_frames = 100;   /* at 60 Hz; the dialog took input after 60-120 frames on the host */
/* Simulation and frame rate (port patch sim-rate): 60 (the game's) or 120,
 * from PORT_SIM_HZ. Also the Present cadence and the refresh rate DXVK
 * reports (runtime/d3d9_thunk.cpp). */
int port_sim_hz = 60;

/* Port patch skip-online-error: confirm the "Connection Problem" dialog the
 * way the player would (one Enter = the dialog's OK), as soon as the dialog
 * takes input. Counted in presented frames, not seconds, so it does not
 * depend on the frame rate; exactly one press, so nothing on the next screen
 * is selected by it. */
uint64_t d3d9t_frames(void);
static void *auto_confirm_thread(void *arg) {
    uint64_t wait = (uint64_t)(uintptr_t)arg, start = d3d9t_frames();
    for (int i = 0; i < 2000 && d3d9t_frames() < start + wait; i++) usleep(10000);
    fprintf(stderr, "PORT_NOTE online error dialog confirmed by the port (frame %llu)\n",
            (unsigned long long)d3d9t_frames());
    port_post_host_input(GU_INPUT_KEY_DOWN, 0, 0, 0x0Du);
    usleep(120000);
    port_post_host_input(GU_INPUT_KEY_UP, 0, 0, 0x0Du);
    return NULL;
}
void port_auto_confirm(unsigned frames) {
    extern int port_sim_hz;
    frames = frames * (unsigned)port_sim_hz / 60u;   /* the dialog animates in game ticks */
    pthread_t t;
    if (pthread_create(&t, NULL, auto_confirm_thread, (void *)(uintptr_t)frames) == 0) pthread_detach(t);
}

/* Public named stop for runtime pieces outside this file. */
void port_named_stop(const char *kind, uint32_t target) {
    named_stop(kind, NULL, NULL, target);
}

/* Direct3D 9 COM method: the guest called through a vtable of the D3D9 layer
 * (runtime/d3d9_thunk.cpp). stdcall, `this` first; the layer says how many
 * argument bytes the method pops. */
#define g_pending_d3d9 (g_cpu.c_pending_trap)
static void static_d3d9_thunk(void) {
    uint32_t trap = g_pending_d3d9, esp = g_esp, ret_va = 0;
    memcpy(&ret_va, g_flat + esp, 4);
    uint32_t eax = g_eax;
    int pop = d3d9t_call(trap, (const uint32_t *)(g_flat + esp + 4), &eax);
    if (pop < 0) {
        fprintf(stderr, "PORT_D3D9_STOP %s (%s) ret=0x%08X\n", d3d9t_trap_name(trap), d3d9t_last_stop(), ret_va);
        named_stop("d3d9-stop", "d3d9.dll", d3d9t_trap_name(trap), trap);
    }
    g_eax = eax;
    g_esp = esp + 4 + (uint32_t)pop;
    g_next = ret_va;
}

/* DirectInput COM method (runtime/guest_dinput.c), same protocol. */
int sb_dinput_call(uint32_t trap, const uint32_t *args, uint32_t *result, const char **what);
static void static_dinput_thunk(void) {
    uint32_t trap = g_pending_d3d9, esp = g_esp, ret_va = 0;
    memcpy(&ret_va, g_flat + esp, 4);
    uint32_t eax = g_eax;
    const char *what = "?";
    int pop = sb_dinput_call(trap, (const uint32_t *)(g_flat + esp + 4), &eax, &what);
    static int trace = -1;
    if (trace < 0) trace = getenv("PORT_TRACE_API") ? atoi(getenv("PORT_TRACE_API")) : 0;
    if (trace < 1 && getenv("PORT_TRACE_INPUT")) trace = 1;
    if (trace) fprintf(stderr, "PORT_DINPUT %s this=%08X args=%08X %08X %08X -> 0x%08X ret=0x%08X\n", what, MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), MEM32(esp + 16), eax, ret_va);
    if (pop < 0) named_stop("dinput-stop", "dinput8.dll", what, trap);
    g_eax = eax;
    g_esp = esp + 4 + (uint32_t)pop;
    g_next = ret_va;
}

/* WMI COM method (runtime/guest_wbem.c), same protocol. */
int sb_wbem_call(uint32_t trap, const uint32_t *args, uint32_t *result, const char **what);
static void static_wbem_thunk(void) {
    uint32_t trap = g_pending_d3d9, esp = g_esp, ret_va = 0;
    memcpy(&ret_va, g_flat + esp, 4);
    uint32_t eax = g_eax;
    const char *what = "?";
    int pop = sb_wbem_call(trap, (const uint32_t *)(g_flat + esp + 4), &eax, &what);
    static int trace = -1;
    if (trace < 0) trace = getenv("PORT_TRACE_INPUT") ? 1 : (getenv("PORT_TRACE_API") ? atoi(getenv("PORT_TRACE_API")) : 0);
    if (trace) fprintf(stderr, "PORT_WBEM %s this=%08X args=%08X %08X %08X %08X -> 0x%08X ret=0x%08X\n", what, MEM32(esp + 4), MEM32(esp + 8), MEM32(esp + 12), MEM32(esp + 16), MEM32(esp + 20), eax, ret_va);
    if (pop < 0) named_stop("wbem-stop", "wbemprox.dll", what, trap);
    g_eax = eax;
    g_esp = esp + 4 + (uint32_t)pop;
    g_next = ret_va;
}

recomp_func_t recomp_lookup(uint32_t va) {
    if (d3d9t_is_trap(va)) { g_pending_d3d9 = va; return static_d3d9_thunk; }
    if (va >= 0x6e3f0000u && va < 0x6e400000u) { g_pending_d3d9 = va; return static_wbem_thunk; }
    if (va >= 0x6e000000u && va < 0x6e400000u) { g_pending_d3d9 = va; return static_dinput_thunk; }
    /* IAT slots and thunk addresses must fall through to recomp_lookup_import. */
    if (va >= THUNK_BASE && va < THUNK_BASE + MAX_IMPORTS * THUNK_STRIDE)
        return NULL;
    if (g_import_dir_va && va >= IMAGE_BASE + g_import_dir_va &&
        va < IMAGE_BASE + g_import_dir_va + g_import_dir_size + 0x1000)
        return NULL;
    uint32_t lo = 0, hi = recomp_dispatch_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (recomp_dispatch_table[mid].address < va) lo = mid + 1;
        else hi = mid;
    }
    if (lo < recomp_dispatch_count && recomp_dispatch_table[lo].address == va)
        return recomp_dispatch_table[lo].func;
    /* Hot-discovery (dev only, PORT_HOT=1): lift this one function now so the
     * run advances through .trace and the needed indirect targets are recorded
     * for a later static re-lift. Returns 0 unless enabled. */
    {
        recomp_func_t hf = port_hot_resolve(va);
        if (hf) return hf;
    }
    /* Not translated. Return a named trap instead of letting the upstream
     * macro print "unresolved VA" and continue with eax = 0 -- a silent stub is
     * exactly what this port must not do. */
    g_last_eip = va;
    return lookup_trap_thunk;
}

recomp_func_t recomp_lookup_import(uint32_t va) {
    uint32_t i = (va - THUNK_BASE) / THUNK_STRIDE;
    if (va >= THUNK_BASE && i < g_import_count && THUNK_BASE + i * THUNK_STRIDE == va &&
        g_imports[i].thunk == va) {
        g_pending_import = (int)i;
        return static_import_thunk;
    }
    if (!static_boot_loaded()) return NULL;
    /* Registry export addresses (module views / trap pool). */
    for (uint32_t k = 0; k < g_import_count; k++)
        if (g_imports[k].thunk == va) {
            g_pending_import = (int)k;
            return static_import_thunk;
        }
    const char *dll, *name;
    if (static_boot_export_at(va, &dll, &name)) return NULL;
    /* The guest found this export on its own (it parses the mapped export
     * tables); bind it now exactly like a GetProcAddress result. */
    uint32_t t = static_resolve_dynamic(dll, name);
    if (t != va) return NULL;
    for (uint32_t k = 0; k < g_import_count; k++)
        if (g_imports[k].thunk == va) {
            g_pending_import = (int)k;
            return static_import_thunk;
        }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* process bring-up                                                    */
/* ------------------------------------------------------------------ */

static int build_module_table(void) {
    /* One registered shim module per imported DLL, in the documented module
     * pool. Handles are opaque guest values; no host pointer is serialized. */
    for (uint32_t i = 0; static_boot_loaded() && i < static_boot_module_count() && g_module_count < 64; i++) {
        uint32_t handle = 0;
        g_modules[g_module_count].name = static_boot_module(i, &handle);
        g_modules[g_module_count].handle = handle;
        g_module_refs[g_module_count] = 0;
        g_module_count++;
    }
    for (uint32_t i = 0; i < g_import_count && g_module_count < 64; i++) {
        const char *dll = g_imports[i].dll;
        int seen = 0;
        for (size_t k = 0; k < g_module_count; k++)
            if (strcasecmp(g_modules[k].name, dll) == 0) { seen = 1; break; }
        if (seen) continue;
        g_modules[g_module_count].name = dll;
        g_modules[g_module_count].handle =
            MODULE_POOL_BASE + (uint32_t)g_module_count * 0x10000u;
        g_module_refs[g_module_count] = 0;
        g_module_count++;
    }
    g_loader.modules = g_modules;
    g_loader.module_count = g_module_count;
    g_loader.exports = NULL;
    g_loader.export_count = 0;
    g_loader.references = g_module_refs;
    /* The main image is loaded; every imported DLL is registered (not resolved
     * to a real PE, which the shim loader does not do). */
    for (size_t i = 0; i < g_module_count; i++) g_module_refs[i] = 1;
    /* With the proven registry: its initial reference counts (a registered but
     * not yet loaded module has no loader-list entry until LoadLibrary). */
    for (uint32_t i = 0; static_boot_loaded() && i < static_boot_module_count() && i < g_module_count; i++)
        g_module_refs[i] = static_boot_module_refs(i);
    return 0;
}

static int setup_teb(void) {
    /* One shared TEB writer (runtime/static_threads.c) so the main thread and
     * every guest thread cannot drift apart. */
    return st_teb_init(g_mem, g_teb, g_teb, STACK_TOP, STACK_BASE, 0x1000u, 0x1004u,
                       g_peb, 0);
}

static int setup_command_line(void) {
    /* C:\BurnoutPR\BurnoutPR.exe as UTF-16LE, matching the PEB virtual path. */
    static const char text[] = "C:\\BurnoutPR\\BurnoutPR.exe";
    uint32_t n = (uint32_t)strlen(text);
    uint8_t *dst = g_flat + COMMAND_BASE;
    for (uint32_t i = 0; i < n; i++) {
        dst[i * 2] = (uint8_t)text[i];
        dst[i * 2 + 1] = 0;
    }
    dst[n * 2] = 0;
    dst[n * 2 + 1] = 0;
    return 0;
}

/* Process-owned ASCII/UTF-16 profile, not the host's environment. Shared with
 * the GetEnvironmentVariableA bridge in runtime/static_bridges.c. */
static const GuestEnvironmentEntry g_env_profile[] = {
        {"SystemRoot", "C:\\Windows"},
        {"windir", "C:\\Windows"},
        {"TEMP", "C:\\Users\\Player\\AppData\\Local\\Temp"},
        {"TMP", "C:\\Users\\Player\\AppData\\Local\\Temp"},
        {"USERPROFILE", "C:\\Users\\Player"},
        {"USERNAME", "Player"},
        {"APPDATA", "C:\\Users\\Player\\AppData\\Roaming"},
        {"LOCALAPPDATA", "C:\\Users\\Player\\AppData\\Local"},
        {"PROGRAMDATA", "C:\\ProgramData"},
        {"PATH", "C:\\Windows\\System32;C:\\Windows"},
};
const GuestEnvironmentEntry *g_env_entries = g_env_profile;
uint32_t g_env_count = (uint32_t)(sizeof g_env_profile / sizeof g_env_profile[0]);

static int setup_environment(void) {
    const GuestEnvironmentEntry *entries = g_env_profile;
    uint32_t count = g_env_count;
    uint8_t block[GE_BLOCK_SIZE];
    uint32_t written = 0;
    int rc = ge_serialize_block(entries, count, block, sizeof block, &written);
    if (rc) return -1;
    memcpy(g_flat + GE_BLOCK_BASE, block, written);
    return 0;
}

static int setup_locale(const char *nls_path) {
    if (gl_initialize(&g_locale, 1252)) return -1;
    if (!nls_path) {
        fprintf(stderr,
                "PORT_NOTE no c_1252.nls supplied: ANSI (CP1252) entry points "
                "will stop instead of guessing bytes\n");
        return 0;
    }
    size_t size = 0;
    uint8_t *nls = read_whole_file(nls_path, &size);
    if (!nls) {
        fprintf(stderr, "PORT_NOTE cannot read %s: %s\n", nls_path,
                strerror(errno));
        return 0;
    }
    int rc = gl_install_cp1252(&g_locale, nls, (uint32_t)size);
    free(nls);
    if (rc) {
        fprintf(stderr, "PORT_NOTE %s is not a usable c_1252.nls\n", nls_path);
        return 0;
    }
    fprintf(stderr, "PORT_LOCALE cp1252 installed from %s\n", nls_path);
    return 0;
}

/* The sort-NLS casing table (lstrcmpiW) sits next to the code-page NLS in the
 * private reference directory. A missing file is a named stop at the first
 * lstrcmpiW, never a guessed comparison. */
static int setup_string_compare(const char *nls_path) {
    if (!nls_path) return 0;
    char path[4096];
    snprintf(path, sizeof path, "%s", nls_path);
    char *slash = strrchr(path, '/');
    if (!slash) return 0;
    snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "sortdefault.nls");
    size_t size = 0;
    uint8_t *nls = read_whole_file(path, &size);
    if (!nls) {
        fprintf(stderr, "PORT_NOTE no sort NLS at %s: lstrcmpiW will stop\n", path);
        return 0;
    }
    int rc = gls_create(nls, (uint32_t)size, &g_strcmp);
    ob_install_sort_keys(nls, size);                /* CompareStringW weights (runtime/orig_bridges.c) */
    free(nls);
    if (rc) {
        g_strcmp = NULL;
        fprintf(stderr, "PORT_NOTE %s is not a usable sort NLS\n", path);
        return 0;
    }
    fprintf(stderr, "PORT_STRCMP sort NLS installed from %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* A12 host_bridges context                                            */
/* ------------------------------------------------------------------ */
/*
 * runtime/win32_api_table.c (the A4 import manifest) carries a `run_*`
 * adapter for every row with implementation == A4_IMPL_C_RUNTIME. Those
 * adapters now live in runtime/host_bridges.c, which reaches the guest CPU
 * through four accessors and the guest memory through two checked accessors.
 * This block is the static image's side of that contract:
 *
 *   * cpu_get/cpu_set operate on the *real* recomp32 globals, so an adapter
 *     that pops its stdcall frame and writes EAX changes exactly the state
 *     the lifted code continues with;
 *   * cpu_state produces the 9-word GPR/EIP snapshot plus the packed EFLAGS
 *     word (the lazy flag model is materialised with recomp_eflags());
 *   * guest_read/guest_write are gm_read/gm_write and longjmp into the
 *     adapter's own setjmp (HB_BEGIN) on a fault, which is how
 *     host_bridges.c reports a guest memory fault;
 *   * the KUSER clock is sampled per call (gst_next) because the static image
 *     has no SharedUserData page to publish into -- documented, not hidden.
 */
static jmp_buf g_hb_fault;
static GuestSystemTime g_hb_clock;
static int g_hb_cpu_token;

static uint32_t hb_cpu_get(void *cpu, enum HbCpuField field) {
    (void)cpu;
    switch (field) {
    case HB_CPU_EAX: return g_eax;
    case HB_CPU_ESP: return g_esp;
    case HB_CPU_EIP: return g_last_eip ? g_last_eip : g_cur_func;
    case HB_CPU_FS_BASE: return g_fs_base;
    }
    return 0;
}
static void hb_cpu_set(void *cpu, enum HbCpuField field, uint32_t value) {
    (void)cpu;
    switch (field) {
    case HB_CPU_EAX: g_eax = value; break;
    case HB_CPU_ESP: g_esp = value; break;
    case HB_CPU_EIP: g_last_eip = value; break;
    case HB_CPU_FS_BASE: g_fs_base = value; break;
    }
}
static void hb_cpu_state(void *cpu, uint32_t *out) {
    (void)cpu;
    out[0] = g_eax; out[1] = g_ecx; out[2] = g_edx; out[3] = g_ebx;
    out[4] = g_esp; out[5] = g_ebp; out[6] = g_esi; out[7] = g_edi;
    out[8] = g_last_eip ? g_last_eip : g_cur_func;
    out[9] = recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 0);
}
static void hb_guest_read(uint32_t address, void *out, uint32_t size) {
    if (gm_read(g_mem, address, out, size)) longjmp(g_hb_fault, 1);
}
static void hb_guest_write(uint32_t address, const void *in, uint32_t size) {
    if (gm_write(g_mem, address, in, size)) longjmp(g_hb_fault, 1);
}
static int hb_clock_next(void *out, uint64_t *ticks) {
    GuestSystemTime next = g_hb_clock;
    int rc = gst_next(&next, ticks);
    if (rc) return rc;
    memcpy(out, &next, sizeof next);
    return GM_OK;
}
static void hb_clock_store(const void *in) {
    /* No SharedUserData page is published by the static image yet, so the
     * consumed clock state is only kept for the next sample. */
    memcpy(&g_hb_clock, in, sizeof g_hb_clock);
}

/* The A4 adapters (runtime/host_bridges.c: GetStringTypeW, LCMapStringEx,
 * MultiByteToWideChar, EncodePointer ...) keep their own NLS and pointer-codec
 * state.  Install the same private reference tables the measurement driver
 * installs (runtime/proven_boot_main.c); a missing table leaves the adapter a
 * named stop, never a guessed classification. */
int initialize_pointer_cookie(uint32_t cookie);
int initialize_locale(uint32_t ansi);
int install_code_page(const uint8_t *bytes, uint32_t size);
int initialize_character_types(const uint8_t *raw, uint32_t size);
int initialize_case_maps(const uint8_t *raw, uint32_t size);
static int setup_host_nls(const char *nls_path) {
    if (initialize_pointer_cookie(1767433348u)) return -1;      /* port/diagnostic-pointer-profile.json */
    if (initialize_locale(1252)) return -1;
    if (!nls_path) return 0;
    size_t n1 = 0, n2 = 0;
    uint8_t *cp = read_whole_file(nls_path, &n1);
    char path[4096];
    snprintf(path, sizeof path, "%s", nls_path);
    char *slash = strrchr(path, '/');
    uint8_t *sort = NULL;
    if (slash) {
        snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "sortdefault.nls");
        sort = read_whole_file(path, &n2);
    }
    int rc = 0;
    if (cp && install_code_page(cp, (uint32_t)n1)) rc = -2;
    if (!rc && sort && initialize_character_types(sort, (uint32_t)n2)) rc = -3;
    if (!rc && sort && initialize_case_maps(sort, (uint32_t)n2)) rc = -4;
    fprintf(stderr, "PORT_HOST_NLS code page %s, character types/case maps %s (rc=%d)\n",
            cp ? "installed" : "missing", sort ? "installed" : "missing", rc);
    free(cp); free(sort);
    return rc;
}

static int setup_host_bridges(void) {
    HostBridgeContext context;
    memset(&context, 0, sizeof context);
    context.memory = g_mem;
    /* The accessors ignore this pointer: the guest CPU of the static image is
     * the recomp32 global register file, not a host struct. hb_set_context()
     * still requires it to be non-NULL, so it carries the TEB address as an
     * opaque non-zero token. */
    context.cpu = &g_hb_cpu_token;
    context.cpu_get = hb_cpu_get;
    context.cpu_set = hb_cpu_set;
    context.cpu_state = hb_cpu_state;
    context.guest_read = hb_guest_read;
    context.guest_write = hb_guest_write;
    context.fault_return = &g_hb_fault;
    context.loader = &g_loader;
    context.image_base = IMAGE_BASE;
    context.image_size = g_image_size;
    context.process_entry = g_entry_va;
    context.image_views = NULL;
    context.image_view_count = 0;
    context.api_sets = NULL;
    context.api_set_count = 0;
    context.environment = g_env_entries;
    context.environment_count = g_env_count;
    context.clock_next = hb_clock_next;
    context.clock_store = hb_clock_store;
    if (hb_set_context(&context)) return GM_UNSUPPORTED;
    /* Same order as the proven host (setup(): reset, then initialise): the
     * reset is what starts the thread clock (GetThreadTimes) and creates the
     * default fiber's FLS table. Nothing has been installed yet at this point. */
    hb_reset_state();
    int rc = hb_initialize_state();
    if (rc) return rc;
    fprintf(stderr, "PORT_HOST_BRIDGES adapters=%u\n", guest_api_bound_count());
    return 0;
}

/* VirtualQuery for what this runtime maps itself: the image (per section),
 * the main and guest-thread stacks and the process heap arena. */
void gvm_set_region_describer(int (*fn)(uint32_t address, uint32_t mbi[7]));
static int describe_region(uint32_t address, uint32_t mbi[7]) {
    uint32_t page = address & ~0xFFFu;
    if (address >= IMAGE_BASE && address < IMAGE_BASE + g_image_size) {
        uint32_t lo = IMAGE_BASE, hi = IMAGE_BASE + 0x1000u, protect = 0x02u;      /* headers: read-only */
        for (uint32_t i = 0; i < g_section_count; i++) {
            uint32_t s0 = IMAGE_BASE + g_sections[i].va;
            uint32_t s1 = s0 + ((g_sections[i].vsize + 0xFFFu) & ~0xFFFu);
            if (address < s0 || address >= s1) continue;
            uint32_t ch = g_sections[i].characteristics;
            int x = (ch & 0x20000000u) != 0, w = (ch & 0x80000000u) != 0;
            protect = x ? (w ? 0x40u : 0x20u) : (w ? 0x04u : 0x02u);
            lo = s0; hi = s1;
            break;
        }
        (void)lo;
        mbi[0] = page; mbi[1] = IMAGE_BASE; mbi[2] = 0x80u; mbi[3] = hi - page;
        mbi[4] = 0x1000u; mbi[5] = protect; mbi[6] = 0x01000000u;                  /* MEM_COMMIT, MEM_IMAGE */
        return 1;
    }
    uint32_t base = 0, end = 0;
    if (address >= STACK_BASE && address < STACK_TOP) { base = STACK_BASE; end = STACK_TOP; }
    else if (address >= ST_GUEST_STACK_ARENA && address < ST_GUEST_STACK_LIMIT) {
        base = ST_GUEST_STACK_ARENA + ((address - ST_GUEST_STACK_ARENA) / ST_GUEST_SLOT_STRIDE) * ST_GUEST_SLOT_STRIDE;
        end = base + ST_GUEST_STACK_SIZE;
        if (address >= end) return 0;                                               /* the slot's TEB page, not stack */
    } else if (address >= GPH_BASE && address < GPH_LIMIT) { base = GPH_BASE; end = GPH_LIMIT; }
    else return 0;
    mbi[0] = page; mbi[1] = base; mbi[2] = 0x04u; mbi[3] = end - page;
    mbi[4] = 0x1000u; mbi[5] = 0x04u; mbi[6] = 0x00020000u;                         /* MEM_COMMIT, PAGE_READWRITE, MEM_PRIVATE */
    return 1;
}

/* Vehicles added by the port-mods overlay (VEHICLES/PORT_ADDED_VEHICLES.TXT,
 * one 64-bit CgsID in hex per line). The port patch grant-added-vehicles hands
 * exactly these ids to the game's own give-vehicle routine (0x00508040), so
 * they are owned from the start as in the Remastered; nothing else is granted.
 * Returns the number of ids; *ids points at lo/hi pairs. */
static char g_mod_root[1024];
int port_added_vehicles(const uint32_t **ids) {
    static uint32_t list[2 * 256]; static int count = -1;
    if (count < 0) {
        count = 0;
        if (g_mod_root[0]) {
            char path[1100]; snprintf(path, sizeof path, "%s/VEHICLES/PORT_ADDED_VEHICLES.TXT", g_mod_root);
            FILE *f = fopen(path, "r"); char line[64];
            while (f && fgets(line, sizeof line, f) && count < 256) {
                if (line[0] == '#' || line[0] == '\n') continue;
                unsigned long long v = strtoull(line, NULL, 16);
                if (!v) continue;
                list[2 * count] = (uint32_t)v; list[2 * count + 1] = (uint32_t)(v >> 32); count++;
            }
            if (f) fclose(f);
            fprintf(stderr, "PORT_MODS %d added vehicles listed\n", count);
        }
    }
    *ids = list;
    return count;
}

/* 1 when <lo,hi> is one of the vehicles the port-mods overlay adds. */
int port_is_added_vehicle(uint32_t lo, uint32_t hi) {
    const uint32_t *ids; int n = port_added_vehicles(&ids);
    for (int i = 0; i < n; i++) if (ids[2 * i] == lo && ids[2 * i + 1] == hi) return 1;
    return 0;
}

static int setup_process(const char *game_root, const char *user_root,
                         const char *nls_path) {
#define STEP(label, expr)                                                     \
    do {                                                                      \
        int _rc = (expr);                                                     \
        if (_rc) {                                                            \
            fprintf(stderr, "PORT_FATAL step %-28s rc=%d\n", label, _rc);      \
            return -1;                                                        \
        }                                                                     \
    } while (0)

    STEP("flat_reserve", flat_reserve());
    STEP("guest_memory_attach", guest_memory_attach());
    STEP("load_image", load_image(game_root));
    if (getenv("PORT_BOOT")) {
        STEP("boot_registry", static_boot_load(getenv("PORT_BOOT"), g_flat));
        fprintf(stderr, "PORT_BOOT registry modules=%u regions=%u from %s\n",
                static_boot_module_count(), static_boot_region_count(), getenv("PORT_BOOT"));
    }
    STEP("parse_imports", parse_imports());
    STEP("build_module_table", build_module_table());
    /* The stack and every other region live inside the single flat mapping
     * created by guest_memory_attach(); there is nothing left to map. */
    STEP("teb", setup_teb());

    STEP("command_line", setup_command_line());
    uint32_t command_bytes = (uint32_t)(strlen("C:\\BurnoutPR\\BurnoutPR.exe") + 1) * 2;
    STEP("gp_initialize",
         gp_initialize(g_mem, IMAGE_BASE, g_image_size, g_entry_va, COMMAND_BASE,
                       command_bytes, &g_loader));
    STEP("loader_lists", static_sync_loader_lists());
    STEP("environment", setup_environment());

    g_heap = gph_create(g_mem);
    if (!g_heap) { fprintf(stderr, "PORT_FATAL gph_create\n"); return -1; }
    STEP("gph_initialize", gph_initialize(g_heap, g_peb));
    g_local = lh_create(g_mem);
    if (!g_local) { fprintf(stderr, "PORT_FATAL lh_create\n"); return -1; }
    STEP("gsti_initialize",
         gsti_initialize(g_mem, g_teb, TLS_VECTOR_BASE, TLS_VECTOR_SLOTS));
    g_tls_process = gt_process_create();
    g_tls_thread = g_tls_process ? gt_thread_create(g_tls_process) : NULL;
    if (!g_tls_thread) { fprintf(stderr, "PORT_FATAL gt_thread_create\n"); return -1; }
    gsrw_initialize(&g_srw);
    g_fls = gfl_create();
    if (!g_fls) { fprintf(stderr, "PORT_FATAL gfl_create\n"); return -1; }
    STEP("gwr_create", gwr_create(&g_registry));
    STEP("setup_locale", setup_locale(nls_path));
    STEP("setup_string_compare", setup_string_compare(nls_path));
    STEP("gpm_create", gpm_create(game_root, user_root, &g_pmap));
    {   /* Read overlay for added content (converted vehicles): PORT_MOD_ROOT or
         * <game_root>/port-mods when that directory exists. */
        char mods[1024]; struct stat mst;
        const char *mr = getenv("PORT_MOD_ROOT");
        if (!mr || !*mr) {
            snprintf(mods, sizeof mods, "%s/port-mods", game_root);
            mr = (stat(mods, &mst) == 0 && S_ISDIR(mst.st_mode)) ? mods : NULL;
        }
        if (mr && strcmp(mr, "0")) {
            gpm_set_mod_root(g_pmap, mr);
            snprintf(g_mod_root, sizeof g_mod_root, "%s", mr);
            fprintf(stderr, "PORT_MODS overlay %s\n", mr);
        }
    }
    STEP("gfa_create", gfa_create(g_pmap, &g_locale, &g_fileapi));

    /* ---- A14: bridge table + context of every wired module ---- */
    uint32_t bridges = sb_build();
    /* A12: the win32_api_table C_RUNTIME adapters need their context before any
     * import can reach them. */
    STEP("host_bridges", setup_host_bridges());
    STEP("host_nls", setup_host_nls(nls_path));

    /* Virtual memory: the arena is created so the gvm_exports are bound, but in
     * the flat model the whole 32-bit space is already mapped, so gvm's
     * per-reservation gm_map() reports GM_OVERLAP and VirtualAlloc answers NULL
     * (+ LastError) exactly like a failed Win32 allocation. That is a documented
     * semantic gap of the flat memory model, not a fabricated success. */
    sb_set_virtual_memory(gvm_create(g_mem, VIRTUAL_ARENA_BASE, VIRTUAL_ARENA_LIMIT));
    gvm_set_region_describer(describe_region);

    /* Guest threads: real pthreads with their own guest stack/TEB/CPU state. */
    gcs_set_multithread(1);
    gsrw_set_multithread(1);
    gta_set_thread_launcher(st_thread_launch);
    st_thread_set_context(g_mem, g_peb, 0x1000u, TLS_VECTOR_BASE, TLS_VECTOR_SLOTS);
    st_thread_set_stop_hook(thread_stop);
    recomp_yield_hook = st_machine_yield;

    /* user32 surface: the host test run has no window, so the model gets a
     * surface size and no guest-call hook. Message dispatch that would have to
     * call a guest WndProc therefore stops honestly. */
    g_user32 = gu_create(g_mem, g_teb);
    if (g_user32) gu_set_surface_size(g_user32, 1280, 720);
#ifndef PORT_PCPU
    /* Window procedures are guest code: deliver messages by calling them. */
    if (g_user32) {
        gu_set_guest_call(g_user32, user32_guest_call, NULL);
        uint32_t heap = 0, block = 0;
        if (!gph_current(g_heap, g_peb, &heap) && !gph_alloc(g_heap, heap, 8, 256, &block) && block)
            gu_set_scratch(g_user32, block, 256);       /* CREATESTRUCT for WM_NCCREATE/WM_CREATE */
    }
#endif
    /* COM/D3D11 object model: arena for vtables and guest objects. */
    diag_install();
    d3d9t_init(g_flat);
    {   /* compiled-shader cache of the D3DX bridge: private, next to the saves */
        char cache[1024];
        snprintf(cache, sizeof cache, "%s/port-shader-cache", user_root);
        d3dxt_init(g_flat, cache);
    }
#ifndef __ANDROID__
    {   /* Host: DXVK's headless WSI takes a size record instead of a window. */
        static struct { int32_t width, height, format; } host_window = { 1280, 720, 0 };
        const char *size = getenv("PORT_WINDOW");
        if (size) sscanf(size, "%dx%d", &host_window.width, &host_window.height);
        d3d9t_set_native_window(&host_window, host_window.width, host_window.height);
    }
#endif
    g_com = gcom_create(g_mem, g_teb);
    if (g_com) gcom_set_arena(g_com, 0x6f000000u, 0x00800000u);
    /* Per-thread CRT context (errno is per thread in Win32). */
    if (!sb_crt()) { fprintf(stderr, "PORT_FATAL gcrt_create\n"); return -1; }
    /* A19: every A16/A19 module context (ole32 apartment, shell32/gdi32,
     * advapi32 overlay JSON in the user root, ws2_32 sockets, winmm, dinput8,
     * XInput, the missing kernel32 semantics). */
    STEP("bridge_contexts", sb_initialize_contexts(user_root));
    { extern void port_prof_init(void); port_prof_init(); }   /* PORT_PROF=<start>:<seconds> */
    {   /* tools/port_patches.py skip-online-login: no EA login prompt at start-up
         * (EA's servers are gone); PORT_ONLINE_LOGIN=1 shows it again. */
        extern int port_skip_online_login;
        const char *login = getenv("PORT_ONLINE_LOGIN");
        port_skip_online_login = !(login && login[0] == '1');
        extern int port_skip_online_args;
        const char *args = getenv("PORT_SKIP_ONLINE_ARGS");
        if (args) port_skip_online_args = (int)strtol(args, NULL, 16);
        extern int port_skip_online_keep;
        const char *keep = getenv("PORT_SKIP_ONLINE_KEEP");
        if (keep) port_skip_online_keep = atoi(keep);
        extern int port_skip_online_error;
        const char *error = getenv("PORT_ONLINE_ERROR");
        port_skip_online_error = port_skip_online_login && !(error && error[0] == '1');
        extern int port_auto_confirm_frames;
        const char *frames = getenv("PORT_AUTO_CONFIRM_FRAMES");
        if (frames) port_auto_confirm_frames = atoi(frames);
    }
    {   /* tools/port_patches.py sim-rate: PORT_SIM_HZ=120 runs the game at 120
         * frames and simulation steps per second. */
        extern int port_sim_hz;
        const char *hz = getenv("PORT_SIM_HZ");
        if (hz && atoi(hz) == 120) port_sim_hz = 120;
        fprintf(stderr, "PORT_SIM_HZ %d\n", port_sim_hz);
    }
    { extern void st_apply_guest_affinity(void); st_apply_guest_affinity(); }   /* main guest thread */
#ifndef PORT_PCPU
    {   /* DirectInput: guest callbacks (EnumDevices/EnumObjects) and the key
         * set of the platform keyboard = the keys the input layer can deliver
         * (runtime/android_input.c owns the AKEYCODE -> DIK table). */
        extern GuestDinput *sb_dinput(void);
        uint8_t keys[256];
        memset(keys, 0, sizeof keys);
        for (uint32_t i = 0; i < ain_akeycode_count(); i++) {
            uint32_t dik = 0, vk = 0;
            ain_akeycode_at(i, &dik, &vk);
            if (dik && dik < 256) keys[dik] = 1;
        }
        gdi_set_guest_call(sb_dinput(), user32_guest_call, NULL);
        gdi_set_keyboard_keys(sb_dinput(), keys);
        /* Input layer: key/touch events feed DirectInput and the window
         * messages; the gamepad goes to XInput controller 0. */
        extern int sb_gamepad_to_xinput(const port_gamepad_state *state);
        extern int sb_gamepad_native(void);
        /* The image reads the pad itself (XInput): no pad -> key translation. */
        ain_default_set_native_gamepad(sb_gamepad_native());
        ain_default_set_targets(sb_dinput(), g_user32);
        ain_set_gamepad_sink(sb_gamepad_to_xinput);
#ifndef __ANDROID__
        input_script_start();
#endif
    }
#endif

    /* IAT: every slot gets its unique thunk address. */
    uint32_t bound = 0;
    for (uint32_t i = 0; i < g_import_count; i++) {
        StaticImport *im = &g_imports[i];
        STEP("iat-slot-write", gm_write(g_mem, im->slot, &im->thunk, 4));
        const StaticBridge *b = sb_find(im->dll, im->name);
        if (b) {
            im->bound = 1;
            im->argbytes = sb_argbytes(b);
            im->bridge = b;
            bound++;
        }
    }
    fprintf(stderr,
            "PORT_IAT slots=%u bound=%u trap=%u thunk_base=0x%08X bridges=%u\n",
            g_import_count, bound, g_import_count - bound, THUNK_BASE, bridges);
    /* Adopt the oracle's initial guest bytes. Pages this runtime also builds
     * (TEB, PEB, parameters, IAT) are reported with their difference count
     * first, so a drift between the two environments is visible. */
    for (uint32_t i = 0; static_boot_loaded() && i < static_boot_region_count(); i++) {
        uint32_t a, n, perms; const uint8_t *bytes;
        static_boot_region(i, &a, &n, &perms, &bytes);
        if (a == 0x50000000u || a == 0x60000000u || a == 0x70000000u) continue;  /* oracle-only trap/stop/GDT pages */
        uint32_t diff = 0, first = 0;
        for (uint32_t k = 0; k < n; k++)
            if (g_flat[a + k] != bytes[k]) { if (!diff) first = a + k; diff++; }
        if (diff)
            fprintf(stderr, "PORT_BOOT region 0x%08X +0x%X adopted: %u bytes differed (first 0x%08X)\n",
                    a, n, diff, first);
        memcpy(g_flat + a, bytes, n);
    }
#undef STEP

    /* CPU seed: same as runtime/startup_host.c setup(). */
    g_esp = STACK_TOP;
    g_eax = g_ebx = g_ecx = g_edx = g_esi = g_edi = g_ebp = 0;
    g_fs_base = g_teb;
    g_fpu_cw = 0x037Fu;
    g_mxcsr = 0x1F80u;
    g_flag_k = FK_NONE;
    g_flag_a = g_flag_b = g_flag_cf = 0;
    g_cur_func = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* public entry                                                        */
/* ------------------------------------------------------------------ */

/* runtime/win32_api_table.c's VirtualProtect row is served by
 * runtime/host_bridges.c (run_virtual_protect); the A10 stub that used to live
 * here was removed when A12 delivered that file. */

/* Test/diagnostic only: give the *calling* host thread a guest stack, TEB and
 * TLS vector so port_selftest_import() can be driven from a second host thread.
 * Every one of those is RECOMP_TLS, so nothing is shared with the main thread.
 * The guest thread launcher (runtime/static_threads.c) does the same thing for
 * real guest threads; this entry point exists for the contention selftest. */
int port_thread_attach(uint32_t esp, uint32_t teb, uint32_t tls_vector) {
    if (!g_mem || !esp || !teb) return GM_UNSUPPORTED;
    if (st_teb_init(g_mem, teb, teb, esp, esp, 0x1000u, 0x2000u, g_peb,
                    tls_vector))
        return GM_UNSUPPORTED;
    g_esp = esp;
    g_teb = teb;
    g_fs_base = teb;
    g_eax = g_ebx = g_ecx = g_edx = g_esi = g_edi = g_ebp = 0;
    g_crt = NULL;              /* per-thread CRT context, created lazily */
    g_tls_thread = NULL;
    return GM_OK;
}

uint32_t port_guest_eip(void) { return g_last_eip ? g_last_eip : g_cur_func; }
uint32_t port_guest_esp(void) { return g_esp; }
uint32_t port_guest_func(void) { return g_cur_func; }
uint32_t port_bound_imports(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_import_count; i++) n += (uint32_t)g_imports[i].bound;
    return n;
}
uint32_t port_total_imports(void) { return g_import_count; }
uint32_t port_bridge_count(void) { return sb_count(); }

void port_set_resolve_unknown_exports(int on) { g_resolve_unknown = on; }
void port_set_trace_exports(int on) { g_trace_exports = on; }

/* The flat 4 GiB reservation. runtime/static_heap.c allocates the guest
 * process heap directly inside it, so lifted code sees the block bytes. */
uint8_t *port_flat_base(void) { return g_flat; }
int port_va_in_image(uint32_t va) { return va >= IMAGE_BASE && va < IMAGE_BASE + g_image_size; }

/* Find a translated function whose body is a bare `ret` (or `xor eax,eax; ret`).
 * The thread selftest uses it as a guest thread start routine so the launcher
 * runs real lifted code. Returns 0 when no such body exists. */
uint32_t port_find_trivial_function(void) {
    for (uint32_t i = 0; i < recomp_dispatch_count; i++) {
        uint32_t va = recomp_dispatch_table[i].address;
        uint8_t b[4] = {0, 0, 0, 0};
        if (va >= IMAGE_BASE + g_image_size) continue;
        memcpy(b, g_flat + va, 4);
        if (b[0] == 0xC3) return va;
        if (b[0] == 0x33 && b[1] == 0xC0 && b[2] == 0xC3) return va;
    }
    return 0;
}

/* Wiring audit: one line per declared import. `unbound` is a named stop, and
 * this listing is what proves that no name the runtime *does* provide is left
 * unbound by mistake. */
void port_dump_imports(void) {
    for (uint32_t i = 0; i < g_import_count; i++) {
        const StaticImport *im = &g_imports[i];
        printf("IMPORT_BIND %-38s %-46s %s\n", im->dll, im->name,
               im->bound ? "bound" : "unbound");
    }
}
uint32_t port_guest_thread_count(void) { return st_thread_count(); }

int port_setup(const char *game_root, const char *user_root,
               const char *nls_path) {
    if (!game_root || !user_root) {
        fprintf(stderr, "PORT_FATAL game_root and user_root are required\n");
        return -1;
    }
    return setup_process(game_root, user_root, nls_path);
}

/* Bring-up helpers for the host driver: raw guest memory access. */
int port_poke(uint32_t va, const void *bytes, uint32_t size) {
    return gm_write(g_mem, va, bytes, size);
}
int port_peek(uint32_t va, void *bytes, uint32_t size) {
    return gm_read(g_mem, va, bytes, size);
}

/* Bring-up helper: call one translated function exactly as the runtime does
 * (push the dummy return address, dispatch through recomp_lookup). The machine
 * lock is taken so the call runs under the same serialisation a guest thread
 * would. */
int port_call_va(uint32_t va) {
    recomp_func_t fn = recomp_lookup(va);
    if (!fn) {
        fprintf(stderr, "PORT_FATAL 0x%08X is not translated\n", va);
        return -1;
    }
    uint32_t saved = g_esp;
    fprintf(stderr, "PORT_CALL 0x%08X esp=0x%08X\n", va, g_esp);
    fflush(stderr);
    st_machine_lock();
    PUSH32(g_esp, RECOMP_RETADDR);
    (void)fn;
    port_run_guest(va, RECOMP_RETADDR);
    st_machine_unlock();
    fprintf(stderr, "PORT_CALL_RETURN 0x%08X eax=0x%08X esp=0x%08X\n", va, g_eax,
            g_esp);
    g_esp = saved;
    return 0;
}

/* Bring-up helper: perform one guest import call the way RECOMP_ICALL does.
 * `args` are the Win32 arguments in source order (stdcall: pushed right to
 * left). Returns 0 when the thunk popped exactly its own arguments, -2 when the
 * pair is not a declared import of this image. */
int port_selftest_import(const char *dll, const char *name, const uint32_t *args,
                         uint32_t argcount, uint32_t *eax_out,
                         uint32_t *esp_out) {
    int idx = -1;
    for (uint32_t i = 0; i < g_import_count; i++)
        if (!strcmp(g_imports[i].dll, dll) && !strcmp(g_imports[i].name, name)) {
            idx = (int)i;
            break;
        }
    if (idx < 0) {
        /* Not a declared import: resolve it dynamically the way GetProcAddress
         * would, so the selftest can exercise every bridge the runtime has. */
        uint32_t thunk = static_resolve_dynamic(dll, name);
        if (!thunk) return -2;
        for (uint32_t i = 0; i < g_import_count; i++)
            if (g_imports[i].thunk == thunk) { idx = (int)i; break; }
        if (idx < 0) return -2;
    }
    uint32_t saved = g_esp;
    for (uint32_t i = argcount; i > 0; i--) PUSH32(g_esp, args[i - 1]);
    recomp_func_t fn = recomp_lookup_import(g_imports[idx].thunk);
    if (!fn) return -3;
    PUSH32(g_esp, RECOMP_RETADDR);
    uint32_t caller = g_cur_func;
    g_cur_func = IMAGE_BASE;    /* a recognisable caller in the trap report */
    st_machine_lock();
    (void)fn;
    port_run_guest(g_imports[idx].thunk, RECOMP_RETADDR);
    st_machine_unlock();
    g_cur_func = caller;
    /* stdcall: the thunk already popped the return address and the arguments.
     * __cdecl (the seven CRT tables): only the return address was popped, so
     * the caller pops its own arguments here -- exactly as the lifted call site
     * would. */
    if (sb_cdecl(g_imports[idx].bridge)) g_esp += argcount * 4;
    if (eax_out) *eax_out = g_eax;
    if (esp_out) *esp_out = g_esp;
    int ok = (g_esp == saved) ? 0 : -4;
    g_esp = saved;
    return ok;
}

/* <game_root>/port.env: KEY=VALUE lines become environment variables (unless
 * already set). This is how a device run is configured -- an Android app has
 * no command line: PORT_OEP, PORT_NLS, PORT_SHADER_CACHE, PORT_TRACE_* ... */
static void load_port_env(const char *game_root) {
    char path[1024], line[1024];
    snprintf(path, sizeof path, "%s/port.env", game_root);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        if (!getenv(line)) { setenv(line, eq + 1, 0); fprintf(stderr, "PORT_ENV %s=%s\n", line, eq + 1); }
    }
    fclose(f);
}

#ifdef __ANDROID__
/* Android front end (android/jni/port_main.c): the surface of the activity is
 * the window Direct3D renders to. Returning 0 takes the window over. */
int port_set_native_window(void *window, int width, int height) {
    d3d9t_set_native_window(window, width, height);
    return 0;
}
#endif

int port_entry_ex(const char *game_root, const char *user_root,
                  const char *nls_path) {
    if (game_root) load_port_env(game_root);
    if (!nls_path) nls_path = getenv("PORT_NLS");
    if (port_setup(game_root, user_root, nls_path)) return -1;

    /* SecuROM/packed originals: the PE entry is in the wrapper (.bind); start at
     * the real OEP in .text instead (PORT_OEP=0x9AA04B for the Ultimate Box).
     * The wrapper is skipped, not emulated -- we run the owned plaintext code
     * from its real entry; nothing is decrypted or faked. */
    {
        const char *oep = getenv("PORT_OEP");
        if (oep && *oep) { g_entry_va = (uint32_t)strtoul(oep, NULL, 0); fprintf(stderr, "PORT_OEP override entry -> 0x%08X\n", g_entry_va); }
    }
    /* The entry point is a translated function like any other; call it through
     * the dispatch table so a missing body is the same named trap. */
    recomp_func_t entry = recomp_lookup(g_entry_va);
    if (!entry) {
        named_stop("entry-missing", "?", "?", g_entry_va);
    }
    fprintf(stderr, "PORT_ENTRY call 0x%08X esp=0x%08X\n", g_entry_va, g_esp);
    fflush(stderr);
    {
        /* Debugging aid: PORT_PROTECT_RANGE=<va>:<size> makes that guest range
         * read-only on the host after loading (e.g. the image's .rdata, which
         * Windows maps read-only), so a stray write faults at the writer
         * instead of silently corrupting data. Whole host pages inside the
         * range only. */
        const char *spec = getenv("PORT_PROTECT_RANGE");
        unsigned long va = 0, size = 0;
        if (spec && sscanf(spec, "%li:%li", (long *)&va, (long *)&size) == 2 && g_flat && size) {
            uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
            uintptr_t lo = ((uintptr_t)g_flat + va + page - 1) & ~(page - 1);
            uintptr_t hi = ((uintptr_t)g_flat + va + size) & ~(page - 1);
            if (hi > lo && mprotect((void *)lo, hi - lo, PROT_READ) == 0)
                fprintf(stderr, "PORT_PROTECT guest 0x%08lX..0x%08lX read-only\n",
                        (unsigned long)(lo - (uintptr_t)g_flat), (unsigned long)(hi - (uintptr_t)g_flat));
            else fprintf(stderr, "PORT_PROTECT failed for %s\n", spec);
        }
    }
    st_machine_lock();
    /* Match the proven model exactly (startup_host.c setup(): esp=0x30080000,
     * eip=entry, no pushed return address). The PE entry stub jumps into .trace
     * and never returns here, so the process ends via ExitProcess / a named
     * stop, not via the sentinel. Pushing a return address shifted the guest's
     * esp 4 bytes below the oracle and propagated into every stack-derived
     * pointer -- the sole cause of the .trace divergence at dispatch ~488. */
    (void)entry;
    port_run_guest(g_entry_va, RECOMP_RETADDR);
    st_machine_unlock();
    fprintf(stderr, "PORT_ENTRY returned eax=0x%08X esp=0x%08X\n", g_eax, g_esp);
    st_thread_join_all(5000);
    return 0;
}

int port_entry(const char *game_root, const char *user_root) {
    return port_entry_ex(game_root, user_root, NULL);
}
