/* A14 -- the single bridge table of the static image.
 *
 * Every export list the guest runtime provides is registered here, so an IAT
 * slot or a GetProcAddress() name resolves to a real C bridge. The table is
 * built once (sb_build) and verified against the module export tables it was
 * derived from: a name or argument-byte count that drifts between a module's
 * own registration list and this table is reported and aborts the build-up
 * instead of silently mis-popping the guest stack.
 *
 * Nothing is faked. A bridge that cannot model an operation returns a GM_* /
 * named stop code; runtime/static_entry.c turns that into PORT_TRAP.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "static_bridges.h"
#include "static_threads.h"
#include "guest_handles.h"
#include "guest_process_query.h"
#include "guest_string_compare.h"
#include "guest_winsock.h"
#include "guest_sync_objects.h"
#include "guest_thread_api.h"
#include "guest_time_convert.h"
#include "guest_virtual_memory.h"
#include "guest_critical.h"
#include "guest_slist.h"
#include "guest_condition.h"
#include "guest_clock.h"
#include "guest_system_time.h"
#include "guest_peb.h"
/* A16 modules (ole32/oleaut32, shell32/gdi32/imm32/powrprof/dbghelp,
 * advapi32 overlay+crypto, ws2_32 sockets, winmm, dinput8) and the A19
 * additions (XInput, the missing kernel32 semantics, the ordinal table). */
#include "guest_ole.h"
#include "guest_shell_gdi.h"
#include "guest_advapi_extra.h"
#include "guest_winsock_net.h"
#include "guest_winmm.h"
#include "guest_dinput.h"
#include "guest_xinput.h"
#include "guest_wbem.h"
#include "guest_kernel_misc.h"
#include "orig_bridges.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int teb_field(uint32_t off, uint32_t *value) {
    return gm_read(g_mem, g_teb + off, value, 4);
}
static int teb_store(uint32_t off, uint32_t value) {
    return gm_write(g_mem, g_teb + off, &value, 4);
}

/* The CRT context is per guest thread (errno is per thread in Win32). */
GuestCrt *sb_crt(void) {
    if (g_crt) return g_crt;
    if (!g_mem || !g_heap) return NULL;
    GuestCrt *crt = NULL;
    if (gcrt_create(g_mem, g_heap, GPH_HANDLE, g_teb, &g_locale, g_fileapi, &crt) || !crt)
        return NULL;
    g_crt = crt;
    return crt;
}

/* Dynamic TLS values are per guest thread in Win32, so each guest thread gets
 * its own block from the shared slot allocator. */
GuestTlsThread *sb_tls_thread(void) {
    if (g_tls_thread) return g_tls_thread;
    if (!g_tls_process) return NULL;
    g_tls_thread = gt_thread_create(g_tls_process);
    return g_tls_thread;
}

/* ------------------------------------------------------------------ */
/* A16/A19 module contexts                                             */
/* ------------------------------------------------------------------ */
/*
 * One instance per module, created by sb_initialize_contexts() from
 * runtime/static_entry.c's port_entry (the user root is needed for the
 * registry overlay). The COM apartment state is per guest thread -- CoInitialize
 * is a per-thread call in Win32 -- so the GuestOle instance is thread-local and
 * created on first use, exactly like the per-thread CRT context above.
 *
 * The winmm guest-call hook (timeSetEvent callbacks) is deliberately NOT
 * installed: a timer callback would have to enter lifted code from a foreign
 * host thread, and the static image has no guest-call path for that. Without a
 * hook guest_winmm.c reports a named stop instead of silently dropping the
 * callback (see port/build/agents/a19-report.md, "offen").
 */
static GuestShellGdi *g_shellgdi;
static GuestAdvapiExtra *g_advapi_extra;
static GuestWinsockNet *g_winsock_net;
static GuestWinmm *g_winmm;
static GuestDinput *g_dinput;
static GuestXinput *g_xinput;
static GuestKernelMisc *g_kmisc;
static RECOMP_TLS GuestOle *g_ole;
static GuestWbem *g_wbem;
/* 1 = the platform gamepad is presented to the image as an attached XInput
 * controller (DirectInput device + Win32_PNPEntity + XInput user 0). */
static int g_gamepad_native;

static GuestOle *sb_ole(void) {
    if (g_ole) return g_ole;
    if (!g_mem || !g_heap) return NULL;
    GuestOle *ole = NULL;
    if (gol_create(g_mem, g_teb, g_heap, &ole) || !ole) return NULL;
    /* COM state is per guest thread; the WMI repository is one per process. */
    if (g_wbem) gol_register_factory(ole, gwb_clsid_locator, gwb_factory, g_wbem);
    g_ole = ole;
    return ole;
}
static int sb_wbem_bstr(void *context, const char *ascii, uint32_t *bstr) {
    (void)context;
    GuestOle *ole = sb_ole();
    if (!ole) return GM_ALLOCATION;
    return gol_alloc_bstr_ascii(ole, ascii, bstr);
}

/* The platform gamepad as the image sees it. The Odin 3's built-in pad is an
 * Xbox-layout controller ("Xbox Wireless Controller" in Android's input
 * list), i.e. exactly what XInput describes. The identity is the one the
 * image itself ships for that class of device: its default controller table
 * has "[DefaultConfig_XBOX 360 For Windows (Controller)]" with product GUID
 * 028E045E-0000-0000-0000-504944564944 (VID 045E, PID 028E). The PnP instance
 * path is this port's own (the image only reads VID_/PID_/IG_ from it). */
#define SB_PAD_VIDPID 0x028E045Eu
#define SB_PAD_NAME "XBOX 360 For Windows (Controller)"
#define SB_PAD_PNP_ID "HID\\VID_045E&PID_028E&IG_00\\PORT&PAD&0&0000"
int sb_gamepad_native(void) { return g_gamepad_native; }

/* Game language. The Ultimate Box reads it at start-up from the value its
 * installer writes: HKLM\SOFTWARE\EA Games\Burnout(TM) Paradise The Ultimate
 * Box\locale (REG_SZ; image 0x00838800 compares it with its own table at
 * 0x00A5B938: pt_br zh_cn zh_tw cs da nl en_us en_uk fi fr_fr de el hu it ja ko
 * pl pt_pt ru es sv no th; unknown -> en_us). Of those it ships 14 language
 * bundles; for the rest it falls back to en_us itself (0x008388D0).
 *
 * PORT_LANGUAGE carries the choice: the game's own code ("de", "fr_fr") or a
 * locale name ("de-DE", "fr_FR", "zh-TW", "en-GB"), which is mapped to the
 * game's code. The Android shell sets it from the app language (system
 * settings -> apps -> Burnout -> language); port.env overrides it. Unset = no
 * value, i.e. the game's default. */
static int sb_language_code(const char *in, char *out, size_t size) {
    static const char *const codes[] = { "pt_br", "zh_cn", "zh_tw", "cs", "da", "nl", "en_us", "en_uk", "fi", "fr_fr",
                                         "de", "el", "hu", "it", "ja", "ko", "pl", "pt_pt", "ru", "es", "sv", "no", "th" };
    char text[32];
    size_t n = 0;
    for (; in[n] && n + 1 < sizeof text; n++) {
        char c = in[n] == '-' ? '_' : in[n];
        text[n] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    text[n] = 0;
    char *dot = strchr(text, '.');
    if (dot) *dot = 0;                                      /* "de_DE.UTF-8" */
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if (!strcmp(text, codes[i])) { snprintf(out, size, "%s", codes[i]); return 1; }
    char lang[4] = {0}, region[8] = {0};
    sscanf(text, "%3[a-z]_%7[a-z]", lang, region);
    const char *code = NULL;
    if (!strcmp(lang, "en")) code = (!strcmp(region, "gb") || !strcmp(region, "ie") || !strcmp(region, "au") ||
                                     !strcmp(region, "nz") || !strcmp(region, "za") || !strcmp(region, "in")) ? "en_uk" : "en_us";
    else if (!strcmp(lang, "fr")) code = "fr_fr";
    else if (!strcmp(lang, "pt")) code = !strcmp(region, "br") ? "pt_br" : "pt_pt";
    else if (!strcmp(lang, "zh")) code = (!strcmp(region, "tw") || !strcmp(region, "hk") || !strcmp(region, "mo") ||
                                          !strcmp(region, "hant")) ? "zh_tw" : "zh_cn";
    else if (!strcmp(lang, "nb") || !strcmp(lang, "nn")) code = "no";
    else
        for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
            if (!strcmp(lang, codes[i])) code = codes[i];
    if (!code) return 0;
    snprintf(out, size, "%s", code);
    return 1;
}
static void sb_install_language(GuestAdvapiExtra *gax) {
    /* port.env (PORT_LANGUAGE) wins over the app language the Android shell
     * reports (PORT_SYSTEM_LANGUAGE). */
    const char *choice = getenv("PORT_LANGUAGE");
    if (!choice || !*choice) choice = getenv("PORT_SYSTEM_LANGUAGE");
    if (!choice || !*choice) return;
    char code[16];
    if (!sb_language_code(choice, code, sizeof code)) {
        fprintf(stderr, "PORT_LANGUAGE '%s': not a language the game knows; its default (en_us) applies\n", choice);
        return;
    }
    int rc = gax_install_string(gax, "HKLM\\SOFTWARE\\EA Games\\Burnout(TM) Paradise The Ultimate Box", "locale", code);
    fprintf(stderr, "PORT_LANGUAGE %s -> locale=%s (%s)\n", choice, code, rc ? "failed" : "installed");
}

/* Create every A16/A19 module context. Idempotent; returns 0 or a GM_* code.
 * `user_root` is the writable host directory the registry overlay JSON lives
 * in (the same root the path projection uses). */
int sb_initialize_contexts(const char *user_root) {
    if (g_kmisc) return GM_OK;
    if (!g_mem) return GM_UNSUPPORTED;
    if (!g_shellgdi && g_pmap) {
        if (gsg_create(g_mem, g_teb, g_pmap, &g_locale, &g_shellgdi))
            return GM_ALLOCATION;
        gsg_set_surface_size(g_shellgdi, 1280, 720);
    }
    if (!g_advapi_extra && g_registry) {
        if (gax_create(g_mem, g_teb, g_registry, &g_locale, user_root,
                       &g_advapi_extra))
            return GM_ALLOCATION;
        /* Guest scratch for the ANSI entry points (RegOpenKeyExA ...): without
         * it every ANSI registry call is the module's NO_SCRATCH stop. */
        uint32_t heap = 0, block = 0;
        if (!gph_current(g_heap, g_peb, &heap) && !gph_alloc(g_heap, heap, 8, GAX_SCRATCH_MIN, &block) && block)
            gax_set_scratch(g_advapi_extra, block, GAX_SCRATCH_MIN);
        sb_install_language(g_advapi_extra);
    }
    if (!g_winsock_net) {
        if (gwn_create(g_mem, g_teb, &g_winsock_net)) return GM_ALLOCATION;
    }
    if (!g_winmm) {
        if (gwm_create(g_mem, g_teb, &g_winmm)) return GM_ALLOCATION;
        /* The host-test audio backend accepts every format and only counts
         * bytes; the Android build replaces it with the AAudio backend. */
        gwm_set_audio_backend(g_winmm, gwm_null_backend(), NULL);
    }
    if (!g_dinput) {
        if (gdi_create(g_mem, g_teb, &g_dinput)) return GM_ALLOCATION;
        /* Guest vtables/objects of the DirectInput model; the method traps are
         * the addresses from GDI_TRAP_BASE up (never mapped as code). */
        gdi_set_arena(g_dinput, 0x6e400000u, 0x000f0000u);
        /* PORT_GAMEPAD=0 runs without a controller (keyboard only). */
        const char *pad = getenv("PORT_GAMEPAD");
        g_gamepad_native = !(pad && pad[0] == '0');
        /* The platform's key-event stream is the system keyboard (no key is
         * down until the input layer reports one). The image finds an XInput
         * pad through DirectInput enumeration + WMI and then polls XInput. */
        GdiHostState host;
        memset(&host, 0, sizeof host);
        host.present = g_gamepad_native ? 5u : 1u;
        host.gamepad_type = GDI_DI8DEVTYPE_GAMEPAD;
        for (uint32_t i = 0; i < 4; i++) host.pov[i] = 0xffffffffu;
        gdi_set_host_state(g_dinput, &host);
        if (g_gamepad_native) gdi_set_gamepad_identity(g_dinput, SB_PAD_VIDPID, SB_PAD_NAME);
    }
    if (!g_wbem) {
        if (gwb_create(g_mem, &g_wbem)) return GM_ALLOCATION;
        if (gwb_set_arena(g_wbem, 0x6e4f0000u, 0x00010000u)) return GM_ALLOCATION;
        gwb_set_bstr_alloc(g_wbem, sb_wbem_bstr, NULL);
        static const char *const entities[] = { SB_PAD_PNP_ID };
        gwb_set_pnp_entities(g_wbem, entities, g_gamepad_native ? 1u : 0u);
    }
    if (!g_xinput) {
        if (gxi_create(g_mem, g_teb, &g_xinput)) return GM_ALLOCATION;
        /* Attached from the start with a neutral state. */
        if (g_gamepad_native) gxi_set_host_state(g_xinput, 0, 0, 0, 0, 0, 0, 0, 0, 1);
    }
    if (!sb_ole()) return GM_ALLOCATION;
    if (gkm_create(g_mem, g_heap, g_pmap, &g_locale, g_strcmp, g_env_entries,
                   g_env_count, &g_kmisc))
        return GM_ALLOCATION;
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* file API (A7 kernel32_file_exports[])                               */
/* ------------------------------------------------------------------ */

#define GA(i) (a[(i)])
#define FILEAPI g_mem, g_teb, g_fileapi

/* The adapters take the stdcall argument array and the result pointer; a
 * bridge that ignores one of them must still compile under -Wextra -Werror. */
#define SB_ADAPT(nm, call)                                                    \
    static int nm(const uint32_t *a, uint32_t *r) {                           \
        (void)a;                                                              \
        (void)r;                                                              \
        return call;                                                          \
    }

SB_ADAPT(sb_create_file_a, gfa_create_file_a(FILEAPI, GA(0), GA(1), GA(2), GA(3), GA(4), GA(5), GA(6), r))
SB_ADAPT(sb_create_file_w, gfa_create_file_w(FILEAPI, GA(0), GA(1), GA(2), GA(3), GA(4), GA(5), GA(6), r))
SB_ADAPT(sb_read_file, gfa_read_file(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), r))
SB_ADAPT(sb_write_file, gfa_write_file(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), r))
SB_ADAPT(sb_set_file_pointer, gfa_set_file_pointer(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_set_file_pointer_ex, gfa_set_file_pointer_ex(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), r))
SB_ADAPT(sb_get_file_size, gfa_get_file_size(g_mem, g_teb, GA(0), GA(1), r))
SB_ADAPT(sb_get_file_size_ex, gfa_get_file_size_ex(g_mem, g_teb, GA(0), GA(1), r))
SB_ADAPT(sb_get_file_attributes_a, gfa_get_file_attributes_a(FILEAPI, GA(0), r))
SB_ADAPT(sb_get_file_attributes_w, gfa_get_file_attributes_w(FILEAPI, GA(0), r))
SB_ADAPT(sb_get_file_attributes_ex_a, gfa_get_file_attributes_ex_a(FILEAPI, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_get_file_attributes_ex_w, gfa_get_file_attributes_ex_w(FILEAPI, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_find_first_file_a, gfa_find_first_file_a(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_find_first_file_w, gfa_find_first_file_w(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_find_next_file_a, gfa_find_next_file_a(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_find_next_file_w, gfa_find_next_file_w(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_find_close, gfa_find_close(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_create_directory_a, gfa_create_directory_a(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_create_directory_w, gfa_create_directory_w(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_delete_file_a, gfa_delete_file_a(FILEAPI, GA(0), r))
SB_ADAPT(sb_delete_file_w, gfa_delete_file_w(FILEAPI, GA(0), r))
SB_ADAPT(sb_get_file_type, gfa_get_file_type(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_flush_file_buffers, gfa_flush_file_buffers(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_get_current_directory_a, gfa_get_current_directory_a(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_get_current_directory_w, gfa_get_current_directory_w(FILEAPI, GA(0), GA(1), r))
SB_ADAPT(sb_set_current_directory_a, gfa_set_current_directory_a(FILEAPI, GA(0), r))
SB_ADAPT(sb_set_current_directory_w, gfa_set_current_directory_w(FILEAPI, GA(0), r))
SB_ADAPT(sb_get_full_path_name_a, gfa_get_full_path_name_a(FILEAPI, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_get_full_path_name_w, gfa_get_full_path_name_w(FILEAPI, GA(0), GA(1), GA(2), GA(3), r))

/* ------------------------------------------------------------------ */
/* CloseHandle: one dispatcher for every handle kind (A14)             */
/* ------------------------------------------------------------------ */

/* The runtime issues handles from four owners: the file/find API
 * (GH_KIND_FILE/GH_KIND_FIND), the sync objects (event/mutex/semaphore), the
 * thread API (GH_KIND_THREAD) and the synthetic process handle 0x1f00 of
 * guest_process_query. A single CloseHandle bridge dispatches on the object
 * kind in the shared handle table, so there is exactly one CloseHandle in the
 * image and no owner can see another owner's handle. */
static int sb_close_handle(const uint32_t *a, uint32_t *r) {
    uint32_t handle = GA(0);
    uint32_t kind = GH_KIND_NONE;
    if (!gh_lookup(handle, GH_KIND_NONE, &kind))
        return gpq_close_handle(g_mem, g_teb, handle, r);
    switch (kind) {
    case GH_KIND_FILE:
    case GH_KIND_FIND:
        return gfa_close_handle(g_mem, g_teb, handle, r);
    case GH_KIND_EVENT:
    case GH_KIND_MUTEX:
    case GH_KIND_SEMAPHORE:
    case GH_KIND_THREAD:
    case GH_KIND_PROCESS:
    case GH_KIND_MAPPING:
    case GH_KIND_TIMER:
    case GH_KIND_OTHER:
        if (!gh_close(handle)) return GM_UNSUPPORTED;
        *r = 1;
        return GM_OK;
    default:
        return GM_UNSUPPORTED;
    }
}

/* ------------------------------------------------------------------ */
/* kernel32 bridges that need no extra module                          */
/* ------------------------------------------------------------------ */

SB_ADAPT(sb_get_last_error, teb_field(0x34, r))
SB_ADAPT(sb_set_last_error, teb_store(0x34, GA(0)))
SB_ADAPT(sb_get_current_process_id, teb_field(0x20, r))
SB_ADAPT(sb_get_current_thread_id, teb_field(0x24, r))
SB_ADAPT(sb_get_current_process, (*r = 0xffffffffu, 0))
SB_ADAPT(sb_get_current_thread, (*r = 0xfffffffeu, 0))
SB_ADAPT(sb_get_process_heap, gph_current(g_heap, g_peb, r))
SB_ADAPT(sb_heap_alloc, gph_alloc(g_heap, GA(0), GA(1), GA(2), r))
/* HeapCreate: the flat model has one arena, so a new heap aliases the process
 * heap (same handle gph_alloc accepts). HeapDestroy/HeapSetInformation are
 * no-op TRUE -- the shared arena is not torn down per CRT heap. Documented
 * flat-model simplification, not a fabricated result. */
SB_ADAPT(sb_heap_create, gph_current(g_heap, g_peb, r))
static int sb_heap_destroy(const uint32_t *a, uint32_t *r) { (void)a; *r = 1u; return GM_OK; }
static int sb_heap_set_information(const uint32_t *a, uint32_t *r) { (void)a; *r = 1u; return GM_OK; }
/* SetHandleCount: a no-op on modern Windows; the CRT calls it and expects the
 * requested count back. */
static int sb_set_handle_count(const uint32_t *a, uint32_t *r) { *r = GA(0); return GM_OK; }
static int sb_heap_free(const uint32_t *a, uint32_t *r) {
    int rc = gph_free(g_heap, GA(0), GA(1), GA(2));
    *r = rc ? 0u : 1u;
    return rc;
}
SB_ADAPT(sb_heap_size, gph_size(g_heap, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_heap_realloc, gph_realloc(g_heap, GA(0), GA(1), GA(2), GA(3), r))
static int sb_virtual_protect(const uint32_t *a, uint32_t *r) {
    int rc = gm_virtual_protect(g_mem, GA(0), GA(1), GA(2), GA(3));
    *r = rc ? 0u : 1u;
    return rc;
}
SB_ADAPT(sb_get_acp, gl_ansi_code_page(&g_locale, r))
SB_ADAPT(sb_is_valid_code_page, gl_valid_code_page(&g_locale, GA(0), r))

static int sb_get_cp_info(const uint32_t *a, uint32_t *r) {
    uint32_t error = 0;
    int rc = gl_cp_info(&g_locale, g_mem, GA(0), GA(1), r, &error);
    if (!rc && !*r) teb_store(0x34, error);
    return rc;
}
static int sb_multi_byte_to_wide_char(const uint32_t *a, uint32_t *r) {
    uint32_t error = 0;
    int rc = gl_mb_to_wide(&g_locale, g_mem, GA(0), GA(1), GA(2), (int32_t)GA(3),
                           GA(4), (int32_t)GA(5), r, &error);
    if (!rc && !*r) teb_store(0x34, error);
    return rc;
}
static int sb_wide_char_to_multi_byte(const uint32_t *a, uint32_t *r) {
    uint32_t error = 0;
    if (getenv("PORT_DBG_WCTMB"))
        fprintf(stderr, "DBG WCTMB cp=%u flags=%#x wstr=%#x cch=%d mb=%#x cb=%d def=%#x used=%#x\n",
                GA(0), GA(1), GA(2), (int32_t)GA(3), GA(4), (int32_t)GA(5), GA(6), GA(7));
    int default_char = -1;
    if (GA(6)) { uint8_t dc = 0; if (gm_read(g_mem, GA(6), &dc, 1)) return GM_UNCOMMITTED; default_char = dc; }
    uint32_t used_default = 0;
    int rc = gl_wide_to_mb(&g_locale, g_mem, GA(0), GA(1), GA(2), (int32_t)GA(3),
                           GA(4), (int32_t)GA(5), default_char, &used_default, r, &error);
    if (!rc && GA(7)) { uint8_t b4[4] = {(uint8_t)used_default, 0, 0, 0}; if (gm_write(g_mem, GA(7), b4, 4)) return GM_UNCOMMITTED; }
    if (!rc && !*r) teb_store(0x34, error);
    return rc;
}
static int sb_sleep(const uint32_t *a, uint32_t *r) {
    (void)r;
    /* Sleep(0): give up the rest of the time slice to the other ready
     * threads; with none ready it returns at once (documented). */
    if (GA(0) == 0) { st_machine_switch(); return GM_OK; }
    int held = st_machine_release();
    struct timespec ts;
    ts.tv_sec = (time_t)(GA(0) / 1000u);
    ts.tv_nsec = (long)((GA(0) % 1000u) * 1000000L);
    nanosleep(&ts, NULL);
    st_machine_reacquire(held);
    return GM_OK;
}
static int sb_output_debug_string_a(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint32_t p = GA(0);
    char buf[512];
    size_t n = 0;
    for (; n + 1 < sizeof buf; n++) {
        uint8_t c = 0;
        if (gm_read(g_mem, p + (uint32_t)n, &c, 1)) break;
        if (!c) break;
        buf[n] = (char)c;
    }
    buf[n] = 0;
    fprintf(stderr, "PORT_ODS %s\n", buf);
    return GM_OK;
}
static int sb_get_module_handle_a(const uint32_t *a, uint32_t *r) {
    uint32_t p = GA(0);
    char name[64];
    size_t n = 0;
    if (!p) { *r = g_image_base; return GM_OK; }
    for (; n + 1 < sizeof name; n++) {
        uint8_t c = 0;
        if (gm_read(g_mem, p + (uint32_t)n, &c, 1)) break;
        if (!c) break;
        name[n] = (char)c;
    }
    name[n] = 0;
    if (sl_get_loaded(&g_loader, name, "BurnoutPR.exe", g_image_base, r))
        return GM_UNSUPPORTED;
    return GM_OK;
}
static int sb_get_module_handle_w(const uint32_t *a, uint32_t *r) {
    uint32_t p = GA(0);
    char name[64];
    size_t n = 0;
    if (!p) { *r = g_image_base; return GM_OK; }
    for (; n + 1 < sizeof name; n++) {
        uint16_t c = 0;
        if (gm_read(g_mem, p + (uint32_t)n * 2, &c, 2)) break;
        if (!c) break;
        if (c > 0x7f) return GM_UNSUPPORTED;
        name[n] = (char)c;
    }
    name[n] = 0;
    if (sl_get_loaded(&g_loader, name, "BurnoutPR.exe", g_image_base, r))
        return GM_UNSUPPORTED;
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* atomics: Interlocked* on g_mem_base guest addresses (A14)           */
/* ------------------------------------------------------------------ */

/* These are real atomics on the guest bytes, not the 1-CPU model. A guest
 * address is validated through gm_check first, then the operation runs on
 * g_mem_base+va with __atomic, so two guest threads on different host threads
 * see the documented x86 semantics (and the lifted code's plain RMW sequence is
 * serialised by the machine lock, see runtime/static_threads.h). */
static uint32_t *atomic_slot(uint32_t address, uint32_t size) {
    if (address & (size - 1)) return NULL;
    if (gm_check(g_mem, address, size, GM_READ | GM_WRITE)) return NULL;
    return (uint32_t *)(void *)((uintptr_t)g_mem_base + (uintptr_t)address);
}
static int sb_interlocked_increment(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_add_fetch(p, 1u, __ATOMIC_SEQ_CST);
    return GM_OK;
}
static int sb_interlocked_decrement(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_sub_fetch(p, 1u, __ATOMIC_SEQ_CST);
    return GM_OK;
}
static int sb_interlocked_exchange(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_exchange_n(p, GA(1), __ATOMIC_SEQ_CST);
    return GM_OK;
}
static int sb_interlocked_exchange_add(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_fetch_add(p, GA(1), __ATOMIC_SEQ_CST);
    return GM_OK;
}
static int sb_interlocked_compare_exchange(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    uint32_t expected = GA(2);
    __atomic_compare_exchange_n(p, &expected, GA(1), 0, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    *r = expected;   /* the previous value, as x86 cmpxchg leaves in EAX */
    return GM_OK;
}
static int sb_interlocked_or(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_fetch_or(p, GA(1), __ATOMIC_SEQ_CST);
    return GM_OK;
}
static int sb_interlocked_and(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_fetch_and(p, GA(1), __ATOMIC_SEQ_CST);
    return GM_OK;
}
static int sb_interlocked_xor(const uint32_t *a, uint32_t *r) {
    uint32_t *p = atomic_slot(GA(0), 4);
    if (!p) return GM_UNSUPPORTED;
    *r = __atomic_fetch_xor(p, GA(1), __ATOMIC_SEQ_CST);
    return GM_OK;
}
static uint64_t *atomic_slot64(uint32_t address) {
    if (address & 7u) return NULL;
    if (gm_check(g_mem, address, 8, GM_READ | GM_WRITE)) return NULL;
    return (uint64_t *)(void *)((uintptr_t)g_mem_base + (uintptr_t)address);
}
static int sb_interlocked_increment64(const uint32_t *a, uint32_t *r) {
    uint64_t *p = atomic_slot64(GA(0));
    if (!p) return GM_UNSUPPORTED;
    uint64_t v = __atomic_add_fetch(p, 1ull, __ATOMIC_SEQ_CST);
    *r = (uint32_t)v;
    return GM_OK;
}
static int sb_interlocked_decrement64(const uint32_t *a, uint32_t *r) {
    uint64_t *p = atomic_slot64(GA(0));
    if (!p) return GM_UNSUPPORTED;
    uint64_t v = __atomic_sub_fetch(p, 1ull, __ATOMIC_SEQ_CST);
    *r = (uint32_t)v;
    return GM_OK;
}
static int sb_interlocked_exchange64(const uint32_t *a, uint32_t *r) {
    uint64_t *p = atomic_slot64(GA(0));
    if (!p) return GM_UNSUPPORTED;
    uint64_t nv = ((uint64_t)GA(2) << 32) | GA(1);
    uint64_t old = __atomic_exchange_n(p, nv, __ATOMIC_SEQ_CST);
    *r = (uint32_t)old;
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* sync objects (A6 gso_exports[])                                     */
/* ------------------------------------------------------------------ */

/* Waits release the machine lock so the guest thread that would signal can
 * actually run (see runtime/static_threads.h). */
SB_ADAPT(sb_create_event, gso_create_event(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_create_event_w, gso_create_event_w(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_set_event, gso_set_event(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_reset_event, gso_reset_event(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_create_mutex, gso_create_mutex(g_mem, g_teb, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_create_mutex_w, gso_create_mutex_w(g_mem, g_teb, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_release_mutex, gso_release_mutex(g_mem, g_teb, GA(0), r))
/* OpenMutexA/W: no OTHER process holds a named mutex in this single-process
 * port, so the open truthfully fails (NULL + ERROR_FILE_NOT_FOUND). This is the
 * standard single-instance "I am the first instance, proceed" path; nothing is
 * faked -- there genuinely is no cross-process object to open. */
static int sb_open_mutex(const uint32_t *a, uint32_t *r) { (void)a; teb_store(0x34, 2u); *r = 0u; return GM_OK; }
SB_ADAPT(sb_create_semaphore, gso_create_semaphore(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_create_semaphore_w, gso_create_semaphore_w(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_release_semaphore, gso_release_semaphore(g_mem, g_teb, GA(0), GA(1), GA(2), r))

/* Waits give the machine to another guest thread only when they really have
 * to block. Each one first asks with a zero timeout while it still holds the
 * machine: an object that is already signalled is taken on the spot (the same
 * result, and the same consumption of an auto-reset event or semaphore count,
 * as the blocking call). Releasing the machine on every call, signalled or
 * not, turned each wait into a forced reschedule behind every other ready
 * thread. */
#define SB_WAIT_TIMEOUT 0x00000102u
static int sb_wait_single(const uint32_t *a, uint32_t *r) {
    uint32_t value = 0;
    int rc = gso_wait_single(g_mem, g_teb, GA(0), 0, &value);
    if (!rc && value == SB_WAIT_TIMEOUT && GA(1) != 0) {
        int held = st_machine_release();
        rc = gso_wait_single(g_mem, g_teb, GA(0), GA(1), &value);
        st_machine_reacquire(held);
    }
    *r = value;
    return rc;
}
static int sb_wait_single_ex(const uint32_t *a, uint32_t *r) {
    uint32_t value = 0;
    int rc = gso_wait_single_ex(g_mem, g_teb, GA(0), 0, GA(2), &value);
    if (!rc && value == SB_WAIT_TIMEOUT && GA(1) != 0) {
        int held = st_machine_release();
        rc = gso_wait_single_ex(g_mem, g_teb, GA(0), GA(1), GA(2), &value);
        st_machine_reacquire(held);
    }
    *r = value;
    return rc;
}
static int sb_wait_multiple(const uint32_t *a, uint32_t *r) {
    uint32_t value = 0;
    int rc = gso_wait_multiple(g_mem, g_teb, GA(0), GA(1), GA(2), 0, &value);
    if (!rc && value == SB_WAIT_TIMEOUT && GA(3) != 0) {
        int held = st_machine_release();
        rc = gso_wait_multiple(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), &value);
        st_machine_reacquire(held);
    }
    *r = value;
    return rc;
}
static int sb_wait_multiple_ex(const uint32_t *a, uint32_t *r) {
    uint32_t value = 0;
    int rc = gso_wait_multiple_ex(g_mem, g_teb, GA(0), GA(1), GA(2), 0, GA(4), &value);
    if (!rc && value == SB_WAIT_TIMEOUT && GA(3) != 0) {
        int held = st_machine_release();
        rc = gso_wait_multiple_ex(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), &value);
        st_machine_reacquire(held);
    }
    *r = value;
    return rc;
}
SB_ADAPT(sb_initialize_critical_section, gso_initialize_critical_section(g_mem, GA(0), 0, r))
SB_ADAPT(sb_initialize_critical_section_and_spin_count,
         gso_initialize_critical_section(g_mem, GA(0), GA(1), r))
static int sb_initialize_critical_section_ex(const uint32_t *a, uint32_t *r) {
    /* InitializeCriticalSectionEx(lpCS, dwSpinCount, dwFlags): the flags are
     * the documented CRITICAL_SECTION_NO_DEBUG_INFO (0x01000000) or 0. */
    int rc = gcs_initialize(g_mem, GA(0), GA(1), GA(2), 1);
    *r = rc ? 0u : 1u;
    return rc;
}
static int sb_delete_critical_section(const uint32_t *a, uint32_t *r) {
    *r = 0;
    return gso_delete_critical_section(g_mem, GA(0));
}

/* ------------------------------------------------------------------ */
/* critical sections / SRW: multi-thread path (A14)                    */
/* ------------------------------------------------------------------ */

/* The 1-CPU models in runtime/guest_critical.c and runtime/guest_srw.c stop on
 * contention. With real guest threads that is no longer acceptable, so the
 * multi-thread mode of those modules is enabled here (gcs_set_multithread /
 * gsrw_set_multithread) and the blocking paths release the machine lock while
 * they wait. */
static int sb_enter_critical_section(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint32_t tid = 0;
    if (teb_field(0x24, &tid)) return GM_UNCOMMITTED;
    /* Free or already owned by this thread (nearly always, with one guest
     * CPU): enter without letting go of the machine. Only a section another
     * guest thread holds makes this thread wait, and only then is the machine
     * handed over. */
    uint32_t entered = 0;
    int rc = gcs_enter(g_mem, GA(0), tid, 1, &entered);
    if (rc || entered) return rc;
    int held = st_machine_release();
    rc = gcs_enter(g_mem, GA(0), tid, 0, NULL);
    st_machine_reacquire(held);
    return rc;
}
static int sb_leave_critical_section(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint32_t tid = 0;
    if (teb_field(0x24, &tid)) return GM_UNCOMMITTED;
    return gcs_leave(g_mem, GA(0), tid);
}
static int sb_try_enter_critical_section(const uint32_t *a, uint32_t *r) {
    uint32_t tid = 0, entered = 0;
    if (teb_field(0x24, &tid)) return GM_UNCOMMITTED;
    int rc = gcs_enter(g_mem, GA(0), tid, 1, &entered);
    *r = entered;
    return rc;
}
SB_ADAPT(sb_initialize_slist_head, gsl_initialize(g_mem, GA(0)))
SB_ADAPT(sb_initialize_condition_variable, gcv_initialize(g_mem, GA(0)))

static int sb_initialize_srw_lock(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint32_t zero = 0;
    return gm_write(g_mem, GA(0), &zero, 4);
}
static int srw_op(const uint32_t *a, uint32_t *r, uint32_t op, int blocks) {
    if (blocks) {
        /* Same rule as critical sections: try first, hand the machine over
         * only when the lock is really held by someone else. */
        uint32_t got = 0;
        uint32_t try_op = op == GSRW_ACQUIRE_EXCLUSIVE ? GSRW_TRY_ACQUIRE_EXCLUSIVE : GSRW_TRY_ACQUIRE_SHARED;
        int rc = gsrw_call(&g_srw, g_mem, try_op, GA(0), &got);
        if (rc || got) { if (!rc) *r = 0; return rc; }
    }
    int held = 0;
    if (blocks) held = st_machine_release();
    int rc = gsrw_call(&g_srw, g_mem, op, GA(0), r);
    if (blocks) st_machine_reacquire(held);
    return rc;
}
SB_ADAPT(sb_acquire_srw_exclusive, srw_op(a, r, GSRW_ACQUIRE_EXCLUSIVE, 1))
SB_ADAPT(sb_acquire_srw_shared, srw_op(a, r, GSRW_ACQUIRE_SHARED, 1))
SB_ADAPT(sb_release_srw_exclusive, srw_op(a, r, GSRW_RELEASE_EXCLUSIVE, 0))
SB_ADAPT(sb_release_srw_shared, srw_op(a, r, GSRW_RELEASE_SHARED, 0))
SB_ADAPT(sb_try_acquire_srw_exclusive, srw_op(a, r, GSRW_TRY_ACQUIRE_EXCLUSIVE, 0))
SB_ADAPT(sb_try_acquire_srw_shared, srw_op(a, r, GSRW_TRY_ACQUIRE_SHARED, 0))

/* ------------------------------------------------------------------ */
/* thread API (A6 gta_exports[])                                       */
/* ------------------------------------------------------------------ */

static int sb_create_thread(const uint32_t *a, uint32_t *r) {
    /* The launcher reports the guest thread id back through this slot so the
     * handle can be bound to it once CreateThread has issued one. */
    int rc = gta_create_thread(g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), r,
                               GA(5));
    if (!rc) {
        uint32_t tid = 0;
        if (GA(5) && !gm_read(g_mem, GA(5), &tid, 4) && tid)
            st_thread_bind_handle(tid, *r);
    }
    return rc;
}
SB_ADAPT(sb_resume_thread, gta_resume_thread(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_suspend_thread, gta_suspend_thread(g_mem, g_teb, GA(0), r))
SB_ADAPT(sb_get_exit_code_thread, gta_get_exit_code_thread(g_mem, g_teb, GA(0), GA(1), r))
SB_ADAPT(sb_exit_thread, gta_exit_thread(g_mem, g_teb, GA(0), r))
static int sb_sleep_ex(const uint32_t *a, uint32_t *r) {
    if (GA(0) == 0) { st_machine_switch(); *r = 0; return GM_OK; }
    int held = st_machine_release();
    int rc = gta_sleep_ex(GA(0), GA(1), r);
    st_machine_reacquire(held);
    return rc;
}
/* SwitchToThread: the machine goes to the other ready guest threads (it used
 * to yield the host thread while keeping the machine, so no guest thread could
 * actually run). Non-zero only when a switch happened. */
static int sb_switch_to_thread(const uint32_t *a, uint32_t *r) {
    (void)a;
    *r = (uint32_t)st_machine_switch();
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* time (A6 gtc_exports[])                                             */
/* ------------------------------------------------------------------ */

/* The project time zone profile is UTC, so the bias is 0 everywhere. */
#define UTC_BIAS_100NS 0

SB_ADAPT(sb_system_time_to_filetime, gtc_system_time_to_filetime(g_mem, g_teb, GA(0), GA(1), r))
SB_ADAPT(sb_filetime_to_system_time, gtc_filetime_to_system_time(g_mem, g_teb, GA(0), GA(1), r))
SB_ADAPT(sb_get_system_time, gtc_get_system_time(g_mem, GA(0), r))
SB_ADAPT(sb_get_local_time, gtc_get_local_time(g_mem, GA(0), UTC_BIAS_100NS, r))
SB_ADAPT(sb_filetime_to_local_filetime,
         gtc_filetime_to_local_filetime(g_mem, g_teb, GA(0), GA(1), UTC_BIAS_100NS, r))
SB_ADAPT(sb_get_tick_count, gtc_get_tick_count(r))
/* GetTickCount64 returns a ULONGLONG in EDX:EAX, so the bridge has to publish
 * EDX as well. The 64-bit value is produced by the same clock function the
 * 32-bit GetTickCount uses, through a private guest scratch cell. */
static uint32_t g_bridge_edx;
static int g_bridge_edx_valid;
static int sb_get_tick_count64(const uint32_t *a, uint32_t *r) {
    (void)a;
    int rc = gtc_get_tick_count64(g_mem, g_scratch, r);
    if (rc) return rc;
    uint32_t high = 0;
    if (gm_read(g_mem, g_scratch + 4, &high, 4)) return GM_UNCOMMITTED;
    g_bridge_edx = high;
    g_bridge_edx_valid = 1;
    return GM_OK;
}
SB_ADAPT(sb_time_get_time, gtc_time_get_time(r))

/* ------------------------------------------------------------------ */
/* virtual memory (A6 gvm_exports[])                                   */
/* ------------------------------------------------------------------ */

static GuestVirtualMemory *g_vm;

GuestVirtualMemory *sb_virtual_memory(void) { return g_vm; }
void sb_set_virtual_memory(GuestVirtualMemory *vm) { g_vm = vm; }

SB_ADAPT(sb_virtual_alloc, gvm_alloc(g_vm, GA(0), GA(1), GA(2), GA(3), r))
SB_ADAPT(sb_virtual_free, gvm_free(g_vm, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_virtual_query, gvm_query(g_vm, GA(0), GA(1), GA(2), r))

/* ------------------------------------------------------------------ */
/* registry (A3), process query (A3), string compare (A3), winsock (A3) */
/* ------------------------------------------------------------------ */

SB_ADAPT(sb_reg_open_key_ex_w, gwr_open(g_registry, g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), r))
SB_ADAPT(sb_reg_query_value_ex_w, gwr_query(g_registry, g_mem, g_teb, GA(0), GA(1), GA(2), GA(3), GA(4), GA(5), r))
/* RegCloseKey: a key the writable overlay issued (e.g. the game's install key
 * with the language value) is closed there; everything else by the projection. */
static int sb_reg_close_key(const uint32_t *a, uint32_t *r) {
    if (g_advapi_extra) {
        if (gax_reg_close_key_if_overlay(g_advapi_extra, GA(0), r)) return GM_OK;
    }
    return gwr_close(g_registry, g_mem, g_teb, GA(0), r);
}

SB_ADAPT(sb_open_process, gpq_open_process(g_mem, g_teb, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_enum_processes, gpq_enumerate_processes(g_mem, g_teb, GA(0), GA(1), GA(2), r))
SB_ADAPT(sb_get_module_base_name_w,
         gpq_module_base_name_w(g_mem, &g_loader, GA(0), GA(1), GA(2), GA(3), r))

static int sb_lstrcmpi_w(const uint32_t *a, uint32_t *r) {
    if (!g_strcmp) return GM_UNSUPPORTED;
    int32_t value = 0;
    int rc = gls_compare(g_strcmp, g_mem, GA(0), GA(1), &value);
    *r = (uint32_t)value;
    return rc;
}
/* WSAStartup on the same provider state as WSACleanup below (the A4 adapter
 * keeps its own reference count, so a startup there never matched a cleanup
 * here and the guest's WSACleanup stopped). */
static int sb_wsa_startup(const uint32_t *a, uint32_t *r) {
    return gws_startup(g_mem, &g_winsock, GA(0), GA(1), r);
}
static int sb_wsa_cleanup(const uint32_t *a, uint32_t *r) {
    (void)a;
    return gws_cleanup(&g_winsock, r);
}

/* ------------------------------------------------------------------ */
/* dynamic TLS (guest_tls.c), local heap (local_heap.c), environment,   */
/* command line, module file name and the high-resolution clock         */
/* ------------------------------------------------------------------ */

static int sb_tls_alloc(const uint32_t *a, uint32_t *r) {
    (void)a;
    GuestTlsThread *t = sb_tls_thread();
    if (!t) return GM_UNSUPPORTED;
    *r = gt_alloc(t);
    return gt_error(t) ? GM_UNSUPPORTED : GM_OK;
}
static int sb_tls_free(const uint32_t *a, uint32_t *r) {
    GuestTlsThread *t = sb_tls_thread();
    if (!t) return GM_UNSUPPORTED;
    *r = gt_free(t, GA(0));
    return GM_OK;
}
static int sb_tls_get_value(const uint32_t *a, uint32_t *r) {
    GuestTlsThread *t = sb_tls_thread();
    if (!t) return GM_UNSUPPORTED;
    *r = gt_get(t, GA(0));
    return GM_OK;
}
static int sb_tls_set_value(const uint32_t *a, uint32_t *r) {
    GuestTlsThread *t = sb_tls_thread();
    if (!t) return GM_UNSUPPORTED;
    *r = gt_set(t, GA(0), GA(1));
    return GM_OK;
}

/* LocalAlloc/LocalFree over the fixed local-block arena (runtime/local_heap.c).
 * LocalFree returns NULL on success, exactly like Win32. */
static int sb_local_alloc(const uint32_t *a, uint32_t *r) {
    if (!g_local) return GM_UNSUPPORTED;
    return lh_alloc(g_local, GA(1), r);
}
static int sb_local_free(const uint32_t *a, uint32_t *r) {
    if (!g_local) return GM_UNSUPPORTED;
    int rc = lh_free(g_local, GA(0));
    *r = 0;
    return rc;
}

/* GetEnvironmentVariableA over the process-owned ASCII profile (never the
 * host's environment). */
static int sb_get_environment_variable_a(const uint32_t *a, uint32_t *r) {
    if (!g_env_entries) return GM_UNSUPPORTED;
    /* A19: the writable SetEnvironmentVariable overlay is consulted first, then
     * the read-only process profile. */
    if (g_kmisc) {
        uint32_t error = 0;
        int rc = gkm_get_env_a(g_kmisc, GA(0), GA(1), GA(2), r, &error);
        if (rc) return rc;
        if (error) teb_store(0x34, error);
        return GM_OK;
    }
    return ge_get_a(g_mem, g_env_entries, g_env_count, GA(0), GA(1), GA(2), 0, r);
}

/* GetCommandLineW: the PEB ProcessParameters CommandLine UNICODE_STRING the
 * process model already serialised (runtime/guest_peb.c writes it at
 * GP_BASE+0x238). */
static int sb_get_command_line_w(const uint32_t *a, uint32_t *r) {
    (void)a;
    uint32_t buffer = 0;
    if (gm_read(g_mem, g_peb + 0x238 + 4, &buffer, 4)) return GM_UNCOMMITTED;
    *r = buffer;
    return GM_OK;
}

/* GetModuleFileNameA: the loaded main image path from the module metadata. */
static int sb_get_module_file_name_a(const uint32_t *a, uint32_t *r) {
    uint32_t error = 0;
    int rc = gp_main_file_name_a(g_mem, g_peb, GA(0), GA(1), GA(2), r, &error);
    if (!rc && !*r) teb_store(0x34, error);
    return rc;
}

/* shell32!CommandLineToArgvW over the guest command line and the local heap. */
static int sb_command_line_to_argv_w(const uint32_t *a, uint32_t *r) {
    if (!g_local) return GM_UNSUPPORTED;
    /* An empty lpCmdLine means "the process command line" (same fallback the
     * proven driver passes: the PEB ProcessParameters CommandLine buffer). */
    uint32_t fallback = 0;
    if (gm_read(g_mem, g_peb + 0x238 + 4, &fallback, 4)) return GM_UNCOMMITTED;
    return gc_argv(g_mem, g_local, GA(0), GA(1), fallback, r);
}

/* DuplicateHandle over the shared handle table (runtime/guest_handles.c). */
/* DuplicateHandle(hSourceProcess, hSource, hTargetProcess, lpTargetHandle,
 * dwDesiredAccess, bInheritHandle, dwOptions). Only this process exists, so
 * both process handles must be the GetCurrentProcess() pseudo-handle (-1);
 * anything else is a named stop. Sources: a handle of the shared table, or the
 * GetCurrentThread() pseudo-handle (-2), which yields a real handle to the
 * calling thread (the game keeps one per thread to tell its main and render
 * threads apart, image 0x008B7940). Access rights are not modelled
 * (DUPLICATE_SAME_ACCESS or an explicit mask both give the same object);
 * DUPLICATE_CLOSE_SOURCE closes a table handle, and is a no-op on the
 * pseudo-handle as documented. */
static int sb_duplicate_handle(const uint32_t *a, uint32_t *r) {
    const uint32_t current_process = 0xFFFFFFFFu, current_thread = 0xFFFFFFFEu;
    const uint32_t close_source = 1u, same_access = 2u;
    if (GA(0) != current_process || GA(2) != current_process) return GM_UNSUPPORTED;
    if (GA(6) & ~(close_source | same_access)) return GM_UNSUPPORTED;
    if (GA(1) == current_process) return GM_UNSUPPORTED;      /* real process handle: not modelled */
    uint32_t duplicate = 0;
    if (GA(1) == current_thread) {
        int rc = gta_open_current_thread(g_mem, g_teb, &duplicate);
        if (rc) return rc;
        if (!duplicate) { teb_store(0x34, 8); *r = 0; return GM_OK; }   /* ERROR_NOT_ENOUGH_MEMORY */
    } else {
        duplicate = gh_duplicate(GA(1));
        if (!duplicate) {
            teb_store(0x34, 6);   /* ERROR_INVALID_HANDLE */
            *r = 0;
            return GM_OK;
        }
        if (GA(6) & close_source) gh_close(GA(1));
    }
    if (GA(3)) {
        int rc = gm_write(g_mem, GA(3), &duplicate, 4);
        if (rc) { gh_close(duplicate); return rc; }
    }
    *r = 1u;
    return GM_OK;
}

/* GetSystemTimeAsFileTime: the same 100 ns clock provider the SYSTEMTIME
 * bridges use (runtime/guest_system_time.c gst_now). */
static int sb_get_system_time_as_file_time(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint64_t ticks = 0;
    if (gst_now(&ticks)) return GM_UNSUPPORTED;
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++) bytes[i] = (uint8_t)(ticks >> (8 * i));
    return gm_write(g_mem, GA(0), bytes, 8);
}

/* QueryPerformanceCounter/Frequency: the shared monotonic counter provider. */
static int sb_query_performance_counter(const uint32_t *a, uint32_t *r) {
    int rc = gc_counter(g_mem, GA(0));
    if (rc) return rc;
    *r = 1u;
    return GM_OK;
}
static int sb_query_performance_frequency(const uint32_t *a, uint32_t *r) {
    int rc = gc_frequency(g_mem, GA(0));
    if (rc) return rc;
    *r = 1u;
    return GM_OK;
}

/* ------------------------------------------------------------------ */
/* LoadLibrary / GetProcAddress                                        */
/* ------------------------------------------------------------------ */

/* LoadLibraryA/W, FreeLibrary and GetProcAddress live in
 * runtime/static_entry.c: it owns the module table, the dynamic import slots
 * and the named stops, so an unresolvable name keeps its specific trap. */
int static_load_library_a(const uint32_t *a, uint32_t *r);
int static_load_library_w(const uint32_t *a, uint32_t *r);
int static_load_library_ex_w(const uint32_t *a, uint32_t *r);
int static_free_library_bridge(const uint32_t *a, uint32_t *r);
int static_get_proc_address(const uint32_t *a, uint32_t *r);
int static_exit_process(const uint32_t *a, uint32_t *r);
int static_raise_exception(const uint32_t *a, uint32_t *r);
int static_rtl_unwind(const uint32_t *a, uint32_t *r);

/* ------------------------------------------------------------------ */
/* the table                                                           */
/* ------------------------------------------------------------------ */

typedef int (*StaticBridgeFn)(const uint32_t *a, uint32_t *result);

enum {
    SB_PLAIN = 0, SB_CRT = 1, SB_USER32 = 2, SB_COM = 3,
    /* A16/A19 module families: the entry pointer is the module's own export
     * record, whose `fn` takes the module instance as its first argument. */
    SB_OLE = 4, SB_SHELLGDI = 5, SB_ADVAPI = 6, SB_WSN = 7, SB_WINMM = 8,
    SB_DINPUT = 9, SB_XINPUT = 10, SB_KMISC = 11
};

struct StaticBridge {
    const char *dll;
    const char *name;
    uint16_t argbytes;
    uint8_t kind;
    uint8_t cdecl_call;
    StaticBridgeFn fn;
    const void *entry;
};

#define SB_MAX 1024u
static StaticBridge g_table[SB_MAX];
static uint32_t g_table_count;

#define B(dll, name, bytes, fn) \
    { dll, name, bytes, SB_PLAIN, 0, fn, NULL }

static const StaticBridge g_static[] = {
    /* --- A7 file API (kernel32_file_exports[]) --- */
    B("kernel32.dll", "CreateFileA", 28, sb_create_file_a),
    B("kernel32.dll", "CreateFileW", 28, sb_create_file_w),
    B("kernel32.dll", "ReadFile", 20, sb_read_file),
    B("kernel32.dll", "WriteFile", 20, sb_write_file),
    B("kernel32.dll", "SetFilePointer", 16, sb_set_file_pointer),
    B("kernel32.dll", "SetFilePointerEx", 20, sb_set_file_pointer_ex),
    B("kernel32.dll", "GetFileSize", 8, sb_get_file_size),
    B("kernel32.dll", "GetFileSizeEx", 8, sb_get_file_size_ex),
    B("kernel32.dll", "GetFileAttributesA", 4, sb_get_file_attributes_a),
    B("kernel32.dll", "GetFileAttributesW", 4, sb_get_file_attributes_w),
    B("kernel32.dll", "GetFileAttributesExA", 12, sb_get_file_attributes_ex_a),
    B("kernel32.dll", "GetFileAttributesExW", 12, sb_get_file_attributes_ex_w),
    B("kernel32.dll", "FindFirstFileA", 8, sb_find_first_file_a),
    B("kernel32.dll", "FindFirstFileW", 8, sb_find_first_file_w),
    B("kernel32.dll", "FindNextFileA", 8, sb_find_next_file_a),
    B("kernel32.dll", "FindNextFileW", 8, sb_find_next_file_w),
    B("kernel32.dll", "FindClose", 4, sb_find_close),
    B("kernel32.dll", "CreateDirectoryA", 8, sb_create_directory_a),
    B("kernel32.dll", "CreateDirectoryW", 8, sb_create_directory_w),
    B("kernel32.dll", "DeleteFileA", 4, sb_delete_file_a),
    B("kernel32.dll", "DeleteFileW", 4, sb_delete_file_w),
    B("kernel32.dll", "GetFileType", 4, sb_get_file_type),
    B("kernel32.dll", "FlushFileBuffers", 4, sb_flush_file_buffers),
    B("kernel32.dll", "GetCurrentDirectoryA", 8, sb_get_current_directory_a),
    B("kernel32.dll", "GetCurrentDirectoryW", 8, sb_get_current_directory_w),
    B("kernel32.dll", "SetCurrentDirectoryA", 4, sb_set_current_directory_a),
    B("kernel32.dll", "SetCurrentDirectoryW", 4, sb_set_current_directory_w),
    B("kernel32.dll", "GetFullPathNameA", 16, sb_get_full_path_name_a),
    B("kernel32.dll", "GetFullPathNameW", 16, sb_get_full_path_name_w),
    B("kernel32.dll", "CloseHandle", 4, sb_close_handle),
    /* --- process / TEB / heap --- */
    B("kernel32.dll", "GetLastError", 0, sb_get_last_error),
    B("kernel32.dll", "SetLastError", 4, sb_set_last_error),
    B("kernel32.dll", "GetCurrentProcessId", 0, sb_get_current_process_id),
    B("kernel32.dll", "GetCurrentThreadId", 0, sb_get_current_thread_id),
    B("kernel32.dll", "GetCurrentProcess", 0, sb_get_current_process),
    B("kernel32.dll", "GetCurrentThread", 0, sb_get_current_thread),
    B("kernel32.dll", "GetModuleHandleA", 4, sb_get_module_handle_a),
    B("kernel32.dll", "GetModuleHandleW", 4, sb_get_module_handle_w),
    B("kernel32.dll", "VirtualProtect", 16, sb_virtual_protect),
    B("kernel32.dll", "LoadLibraryA", 4, static_load_library_a),
    B("kernel32.dll", "LoadLibraryW", 4, static_load_library_w),
    B("kernel32.dll", "LoadLibraryExW", 12, static_load_library_ex_w),
    B("kernel32.dll", "FreeLibrary", 4, static_free_library_bridge),
    B("kernel32.dll", "GetProcAddress", 8, static_get_proc_address),
    B("kernel32.dll", "GetProcessHeap", 0, sb_get_process_heap),
    B("kernel32.dll", "HeapAlloc", 12, sb_heap_alloc),
    B("kernel32.dll", "HeapFree", 12, sb_heap_free),
    B("kernel32.dll", "HeapSize", 12, sb_heap_size),
    B("kernel32.dll", "HeapReAlloc", 16, sb_heap_realloc),
    B("kernel32.dll", "HeapCreate", 12, sb_heap_create),
    B("kernel32.dll", "HeapDestroy", 4, sb_heap_destroy),
    B("kernel32.dll", "HeapSetInformation", 16, sb_heap_set_information),
    B("kernel32.dll", "SetHandleCount", 4, sb_set_handle_count),
    B("kernel32.dll", "OutputDebugStringA", 4, sb_output_debug_string_a),
    B("kernel32.dll", "ExitProcess", 4, static_exit_process),
    /* SEH over the guest's own handler chain (runtime/static_entry.c) */
    B("kernel32.dll", "RaiseException", 16, static_raise_exception),
    B("kernel32.dll", "RtlUnwind", 16, static_rtl_unwind),
    /* --- dynamic TLS (guest_tls.c) --- */
    B("kernel32.dll", "TlsAlloc", 0, sb_tls_alloc),
    B("kernel32.dll", "TlsFree", 4, sb_tls_free),
    B("kernel32.dll", "TlsGetValue", 4, sb_tls_get_value),
    B("kernel32.dll", "TlsSetValue", 8, sb_tls_set_value),
    /* --- local heap, environment, command line, module path --- */
    B("kernel32.dll", "LocalAlloc", 8, sb_local_alloc),
    B("kernel32.dll", "LocalFree", 4, sb_local_free),
    B("kernel32.dll", "GetEnvironmentVariableA", 12, sb_get_environment_variable_a),
    B("kernel32.dll", "GetCommandLineW", 0, sb_get_command_line_w),
    B("kernel32.dll", "GetModuleFileNameA", 12, sb_get_module_file_name_a),
    B("kernel32.dll", "DuplicateHandle", 28, sb_duplicate_handle),
    B("shell32.dll", "CommandLineToArgvW", 8, sb_command_line_to_argv_w),
    /* --- clock providers already in the runtime --- */
    B("kernel32.dll", "GetSystemTimeAsFileTime", 4, sb_get_system_time_as_file_time),
    B("kernel32.dll", "QueryPerformanceCounter", 4, sb_query_performance_counter),
    B("kernel32.dll", "QueryPerformanceFrequency", 4, sb_query_performance_frequency),
    /* --- code pages (CP1252 profile only) --- */
    B("kernel32.dll", "GetACP", 0, sb_get_acp),
    B("kernel32.dll", "IsValidCodePage", 4, sb_is_valid_code_page),
    B("kernel32.dll", "GetCPInfo", 8, sb_get_cp_info),
    B("kernel32.dll", "MultiByteToWideChar", 24, sb_multi_byte_to_wide_char),
    B("kernel32.dll", "WideCharToMultiByte", 32, sb_wide_char_to_multi_byte),  /* 8 stdcall args = 32 bytes (was 28, under-cleaned esp by 4) */
    /* --- atomics (real __atomic on the guest bytes) --- */
    B("kernel32.dll", "InterlockedIncrement", 4, sb_interlocked_increment),
    B("kernel32.dll", "InterlockedDecrement", 4, sb_interlocked_decrement),
    B("kernel32.dll", "InterlockedExchange", 8, sb_interlocked_exchange),
    B("kernel32.dll", "InterlockedExchangeAdd", 8, sb_interlocked_exchange_add),
    B("kernel32.dll", "InterlockedCompareExchange", 12, sb_interlocked_compare_exchange),
    B("kernel32.dll", "InterlockedOr", 8, sb_interlocked_or),
    B("kernel32.dll", "InterlockedAnd", 8, sb_interlocked_and),
    B("kernel32.dll", "InterlockedXor", 8, sb_interlocked_xor),
    B("kernel32.dll", "InterlockedIncrement64", 4, sb_interlocked_increment64),
    B("kernel32.dll", "InterlockedDecrement64", 4, sb_interlocked_decrement64),
    B("kernel32.dll", "InterlockedExchange64", 12, sb_interlocked_exchange64),
    /* --- A6 sync objects (gso_exports[]) --- */
    B("kernel32.dll", "CreateEventA", 16, sb_create_event),
    B("kernel32.dll", "CreateEventW", 16, sb_create_event_w),
    B("kernel32.dll", "SetEvent", 4, sb_set_event),
    B("kernel32.dll", "ResetEvent", 4, sb_reset_event),
    B("kernel32.dll", "CreateMutexA", 12, sb_create_mutex),
    B("kernel32.dll", "CreateMutexW", 12, sb_create_mutex_w),
    B("kernel32.dll", "ReleaseMutex", 4, sb_release_mutex),
    B("kernel32.dll", "OpenMutexA", 12, sb_open_mutex),
    B("kernel32.dll", "OpenMutexW", 12, sb_open_mutex),
    B("kernel32.dll", "CreateSemaphoreA", 16, sb_create_semaphore),
    B("kernel32.dll", "CreateSemaphoreW", 16, sb_create_semaphore_w),
    B("kernel32.dll", "ReleaseSemaphore", 12, sb_release_semaphore),
    B("kernel32.dll", "WaitForSingleObject", 8, sb_wait_single),
    B("kernel32.dll", "WaitForSingleObjectEx", 12, sb_wait_single_ex),
    B("kernel32.dll", "WaitForMultipleObjects", 16, sb_wait_multiple),
    B("kernel32.dll", "WaitForMultipleObjectsEx", 20, sb_wait_multiple_ex),
    /* --- critical sections / SRW (A3 + A14 multi-thread) --- */
    B("kernel32.dll", "InitializeCriticalSection", 4, sb_initialize_critical_section),
    B("kernel32.dll", "InitializeCriticalSectionAndSpinCount", 8,
      sb_initialize_critical_section_and_spin_count),
    B("kernel32.dll", "InitializeCriticalSectionEx", 12, sb_initialize_critical_section_ex),
    B("kernel32.dll", "EnterCriticalSection", 4, sb_enter_critical_section),
    B("kernel32.dll", "LeaveCriticalSection", 4, sb_leave_critical_section),
    B("kernel32.dll", "TryEnterCriticalSection", 4, sb_try_enter_critical_section),
    B("kernel32.dll", "DeleteCriticalSection", 4, sb_delete_critical_section),
    B("kernel32.dll", "InitializeSListHead", 4, sb_initialize_slist_head),
    B("kernel32.dll", "InitializeConditionVariable", 4, sb_initialize_condition_variable),
    B("kernel32.dll", "InitializeSRWLock", 4, sb_initialize_srw_lock),
    B("kernel32.dll", "AcquireSRWLockExclusive", 4, sb_acquire_srw_exclusive),
    B("kernel32.dll", "AcquireSRWLockShared", 4, sb_acquire_srw_shared),
    B("kernel32.dll", "ReleaseSRWLockExclusive", 4, sb_release_srw_exclusive),
    B("kernel32.dll", "ReleaseSRWLockShared", 4, sb_release_srw_shared),
    B("kernel32.dll", "TryAcquireSRWLockExclusive", 4, sb_try_acquire_srw_exclusive),
    B("kernel32.dll", "TryAcquireSRWLockShared", 4, sb_try_acquire_srw_shared),
    /* --- A6 thread API (gta_exports[]) --- */
    B("kernel32.dll", "CreateThread", 24, sb_create_thread),
    B("kernel32.dll", "ResumeThread", 4, sb_resume_thread),
    B("kernel32.dll", "SuspendThread", 4, sb_suspend_thread),
    B("kernel32.dll", "GetExitCodeThread", 8, sb_get_exit_code_thread),
    B("kernel32.dll", "ExitThread", 4, sb_exit_thread),
    B("kernel32.dll", "Sleep", 4, sb_sleep),
    B("kernel32.dll", "SleepEx", 8, sb_sleep_ex),
    B("kernel32.dll", "SwitchToThread", 0, sb_switch_to_thread),
    /* --- A6 time (gtc_exports[]) --- */
    B("kernel32.dll", "GetSystemTime", 4, sb_get_system_time),
    B("kernel32.dll", "GetLocalTime", 4, sb_get_local_time),
    B("kernel32.dll", "SystemTimeToFileTime", 8, sb_system_time_to_filetime),
    B("kernel32.dll", "FileTimeToSystemTime", 8, sb_filetime_to_system_time),
    B("kernel32.dll", "FileTimeToLocalFileTime", 8, sb_filetime_to_local_filetime),
    B("kernel32.dll", "GetTickCount", 0, sb_get_tick_count),
    B("kernel32.dll", "GetTickCount64", 0, sb_get_tick_count64),
    B("winmm.dll", "timeGetTime", 0, sb_time_get_time),
    /* --- A6 virtual memory (gvm_exports[]) --- */
    B("kernel32.dll", "VirtualAlloc", 16, sb_virtual_alloc),
    B("kernel32.dll", "VirtualFree", 12, sb_virtual_free),
    B("kernel32.dll", "VirtualQuery", 12, sb_virtual_query),
    /* --- A3 registry --- */
    B("advapi32.dll", "RegOpenKeyExW", 20, sb_reg_open_key_ex_w),
    B("advapi32.dll", "RegQueryValueExW", 24, sb_reg_query_value_ex_w),
    B("advapi32.dll", "RegCloseKey", 4, sb_reg_close_key),
    /* --- A3 process query --- */
    B("kernel32.dll", "OpenProcess", 12, sb_open_process),
    B("psapi.dll", "K32EnumProcesses", 12, sb_enum_processes),
    B("kernel32.dll", "K32EnumProcesses", 12, sb_enum_processes),
    B("psapi.dll", "K32GetModuleBaseNameW", 20, sb_get_module_base_name_w),
    B("kernel32.dll", "K32GetModuleBaseNameW", 20, sb_get_module_base_name_w),
    /* --- A3 string compare / winsock --- */
    B("kernel32.dll", "lstrcmpiW", 8, sb_lstrcmpi_w),
    B("ws2_32.dll", "WSAStartup", 8, sb_wsa_startup),
    B("ws2_32.dll", "WSACleanup", 0, sb_wsa_cleanup),
};

/* Append one family of module export records generically. */
static void append_family(const void *table, uint32_t count, uint32_t stride,
                          uint32_t kind, size_t dll_off, size_t name_off,
                          size_t bytes_off) {
    const uint8_t *p = (const uint8_t *)table;
    for (uint32_t i = 0; i < count && g_table_count < SB_MAX; i++, p += stride) {
        StaticBridge *b = &g_table[g_table_count++];
        b->dll = *(const char *const *)(p + dll_off);
        b->name = *(const char *const *)(p + name_off);
        b->argbytes = (uint16_t)*(const uint32_t *)(p + bytes_off);
        b->kind = (uint8_t)kind;
        b->cdecl_call = (kind == SB_CRT) ? 1 : 0;
        b->fn = NULL;
        b->entry = p;
    }
}

typedef struct { const char *dll; const char *name; uint32_t bytes; } SbCheckRow;

/* Verify one module export table against the built table: every entry must be
 * present with the same argument-byte count. Returns 0 or the number of
 * mismatches (reported on stderr). */
static uint32_t verify_family(const char *what, const void *table, uint32_t count,
                              uint32_t stride, size_t dll_off, size_t name_off,
                              size_t bytes_off) {
    const uint8_t *p = (const uint8_t *)table;
    uint32_t bad = 0;
    for (uint32_t i = 0; i < count; i++, p += stride) {
        const char *dll = *(const char *const *)(p + dll_off);
        const char *name = *(const char *const *)(p + name_off);
        uint32_t bytes = *(const uint32_t *)(p + bytes_off);
        const StaticBridge *b = sb_find(dll, name);
        if (!b) {
            fprintf(stderr, "PORT_BRIDGE_MISSING %s %s!%s\n", what, dll, name);
            bad++;
        } else if (b->argbytes != bytes) {
            fprintf(stderr, "PORT_BRIDGE_ARGBYTES %s %s!%s table=%u bridge=%u\n",
                    what, dll, name, bytes, (unsigned)b->argbytes);
            bad++;
        }
    }
    return bad;
}

static int built;

uint32_t sb_build(void) {
    if (built) return g_table_count;
    built = 1;
    for (size_t i = 0; i < sizeof g_static / sizeof g_static[0]; i++)
        g_table[g_table_count++] = g_static[i];
    /* Original-image additions (runtime/orig_bridges.c); a name g_static already
     * serves keeps its existing bridge (first match wins in sb_find). */
    for (uint32_t i = 0; i < orig_bridge_count && g_table_count < SB_MAX; i++) {
        StaticBridge *b = &g_table[g_table_count++];
        b->dll = orig_bridges[i].dll; b->name = orig_bridges[i].name;
        b->argbytes = orig_bridges[i].argbytes; b->kind = SB_PLAIN; b->cdecl_call = 0;
        b->fn = orig_bridges[i].fn; b->entry = NULL;
    }
    /* ws2_32 for the flat model (runtime/orig_winsock.c). Listed before the
     * module families so these rows win over the older single-TEB socket
     * model: one handle space for every socket call. */
    for (uint32_t i = 0; i < orig_winsock_bridge_count && g_table_count < SB_MAX; i++) {
        StaticBridge *b = &g_table[g_table_count++];
        b->dll = orig_winsock_bridges[i].dll; b->name = orig_winsock_bridges[i].name;
        b->argbytes = orig_winsock_bridges[i].argbytes; b->kind = SB_PLAIN; b->cdecl_call = 0;
        b->fn = orig_winsock_bridges[i].fn; b->entry = NULL;
    }
    /* gso/gta/gtc/gvm have per-function signatures, so their adapters are the
     * explicit rows above; verify_family() below proves that every entry of
     * those tables is present with the same argument-byte count. */
    append_family(gu_user32_exports, gu_user32_export_count, sizeof(GuUser32Export),
                  SB_USER32, offsetof(GuUser32Export, dll),
                  offsetof(GuUser32Export, name),
                  offsetof(GuUser32Export, stdcall_arg_bytes));
    append_family(gu_com_exports, gu_com_export_count, sizeof(GuComExport), SB_COM,
                  offsetof(GuComExport, dll), offsetof(GuComExport, name),
                  offsetof(GuComExport, stdcall_arg_bytes));
    append_family(crt_string_exports, crt_string_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));
    append_family(crt_stdio_exports, crt_stdio_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));
    append_family(crt_convert_exports, crt_convert_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));
    append_family(crt_heap_exports, crt_heap_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));
    append_family(crt_misc_exports, crt_misc_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));
    append_family(vcruntime_exports, vcruntime_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));
    append_family(msvcp_exports, msvcp_export_count, sizeof(GcrtExport),
                  SB_CRT, offsetof(GcrtExport, dll), offsetof(GcrtExport, name),
                  offsetof(GcrtExport, stdcall_arg_bytes));

    /* --- A16 module families (all use the same record layout) --- */
#define SB_APPEND_A16(table, count, type, kind)                              \
    append_family(table, count, sizeof(type), kind, offsetof(type, dll),     \
                  offsetof(type, name), offsetof(type, stdcall_arg_bytes))
    SB_APPEND_A16(ole32_exports, ole32_export_count, GuestOleExport, SB_OLE);
    SB_APPEND_A16(oleaut32_exports, oleaut32_export_count, GuestOleExport, SB_OLE);
    SB_APPEND_A16(shell32_exports, shell32_export_count, GuestShellGdiExport,
                  SB_SHELLGDI);
    SB_APPEND_A16(gdi32_exports, gdi32_export_count, GuestShellGdiExport,
                  SB_SHELLGDI);
    SB_APPEND_A16(misc_extra_exports, misc_extra_export_count,
                  GuestShellGdiExport, SB_SHELLGDI);
    SB_APPEND_A16(advapi32_extra_exports, advapi32_extra_export_count,
                  GuestAdvapiExtraExport, SB_ADVAPI);
    SB_APPEND_A16(ws2_32_net_exports, ws2_32_net_export_count,
                  GuestWinsockNetExport, SB_WSN);
    SB_APPEND_A16(winmm_exports, winmm_export_count, GuestWinmmExport, SB_WINMM);
    SB_APPEND_A16(dinput8_exports, dinput8_export_count, GuestDinputExport,
                  SB_DINPUT);
    /* --- A19 families --- */
    SB_APPEND_A16(xinput1_3_exports, xinput1_3_export_count, GuestXinputExport,
                  SB_XINPUT);
    SB_APPEND_A16(kernel32_misc_exports, kernel32_misc_export_count,
                  GuestKernelMiscExport, SB_KMISC);
#undef SB_APPEND_A16

    /* Cross-check the module tables against this table. */
    uint32_t bad = 0;
    bad += verify_family("gso", gso_exports, gso_export_count, sizeof(GsoExport),
                         offsetof(GsoExport, dll), offsetof(GsoExport, name),
                         offsetof(GsoExport, stdcall_arg_bytes));
    bad += verify_family("gta", gta_exports, gta_export_count, sizeof(GtaExport),
                         offsetof(GtaExport, dll), offsetof(GtaExport, name),
                         offsetof(GtaExport, stdcall_arg_bytes));
    bad += verify_family("gtc", gtc_exports, gtc_export_count, sizeof(GtcExport),
                         offsetof(GtcExport, dll), offsetof(GtcExport, name),
                         offsetof(GtcExport, stdcall_arg_bytes));
    bad += verify_family("gvm", gvm_exports, gvm_export_count, sizeof(GvmExport),
                         offsetof(GvmExport, dll), offsetof(GvmExport, name),
                         offsetof(GvmExport, stdcall_arg_bytes));
    bad += verify_family("user32", gu_user32_exports, gu_user32_export_count,
                         sizeof(GuUser32Export), offsetof(GuUser32Export, dll),
                         offsetof(GuUser32Export, name),
                         offsetof(GuUser32Export, stdcall_arg_bytes));
    bad += verify_family("com", gu_com_exports, gu_com_export_count,
                         sizeof(GuComExport), offsetof(GuComExport, dll),
                         offsetof(GuComExport, name),
                         offsetof(GuComExport, stdcall_arg_bytes));
    /* A16/A19: every entry of every module list must be present with the same
     * argument-byte count. A drift here would mis-pop the guest stack. */
#define SB_VERIFY_A16(what, table, count, type)                              \
    bad += verify_family(what, table, count, sizeof(type), offsetof(type, dll),\
                         offsetof(type, name),                               \
                         offsetof(type, stdcall_arg_bytes))
    SB_VERIFY_A16("ole32", ole32_exports, ole32_export_count, GuestOleExport);
    SB_VERIFY_A16("oleaut32", oleaut32_exports, oleaut32_export_count,
                  GuestOleExport);
    SB_VERIFY_A16("shell32", shell32_exports, shell32_export_count,
                  GuestShellGdiExport);
    SB_VERIFY_A16("gdi32", gdi32_exports, gdi32_export_count,
                  GuestShellGdiExport);
    SB_VERIFY_A16("misc_extra", misc_extra_exports, misc_extra_export_count,
                  GuestShellGdiExport);
    SB_VERIFY_A16("advapi32_extra", advapi32_extra_exports,
                  advapi32_extra_export_count, GuestAdvapiExtraExport);
    SB_VERIFY_A16("ws2_32_net", ws2_32_net_exports, ws2_32_net_export_count,
                  GuestWinsockNetExport);
    SB_VERIFY_A16("winmm", winmm_exports, winmm_export_count, GuestWinmmExport);
    SB_VERIFY_A16("dinput8", dinput8_exports, dinput8_export_count,
                  GuestDinputExport);
    SB_VERIFY_A16("xinput1_3", xinput1_3_exports, xinput1_3_export_count,
                  GuestXinputExport);
    SB_VERIFY_A16("kernel32_misc", kernel32_misc_exports,
                  kernel32_misc_export_count, GuestKernelMiscExport);
#undef SB_VERIFY_A16
    fprintf(stderr, "PORT_BRIDGES total=%u verify_failures=%u\n", g_table_count, bad);
    return g_table_count;
}

uint32_t sb_count(void) { return g_table_count; }

static int dll_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

const StaticBridge *sb_find(const char *dll, const char *name) {
    if (!dll || !name) return NULL;
    for (uint32_t i = 0; i < g_table_count; i++) {
        if (!strcmp(g_table[i].name, name) && dll_equal(g_table[i].dll, dll))
            return &g_table[i];
    }
    return NULL;
}
const char *sb_dll(const StaticBridge *b) { return b ? b->dll : NULL; }
const char *sb_name(const StaticBridge *b) { return b ? b->name : NULL; }
uint32_t sb_argbytes(const StaticBridge *b) { return b ? b->argbytes : 0; }
int sb_cdecl(const StaticBridge *b) { return b ? b->cdecl_call : 0; }

/* ------------------------------------------------------------------ */
/* invocation                                                          */
/* ------------------------------------------------------------------ */

static int sb_invoke_inner(const StaticBridge *b, uint32_t esp, uint32_t *eax, uint32_t *edx,
                           int *st0_valid, uint64_t *st0_bits);
const char *port_act_enter(const char *what);      /* runtime/port_prof.c */
void port_act_leave(const char *previous);
void port_act_esp(uint32_t esp);
int port_stats_active(void);
void port_bridge_stat(const char *name, uint64_t ns);
void port_wait_stat(uint32_t caller, uint32_t timeout, int timed_out, uint64_t ns);
int sb_invoke(const StaticBridge *b, uint32_t esp, uint32_t *eax, uint32_t *edx,
              int *st0_valid, uint64_t *st0_bits) {
    const char *previous = port_act_enter(b ? b->name : "(bridge)");
    port_act_esp(esp);
    {   /* PORT_STACK_ON=<bridge name>: guest call chain of the first calls
         * (return addresses into the image found on the guest stack). */
        static const char *stack_on;
        static int stack_left = -1;
        if (stack_left < 0) { stack_on = getenv("PORT_STACK_ON"); stack_left = stack_on ? 4 : 0; }
        if (stack_left > 0 && b && !strcmp(b->name, stack_on)) {
            stack_left--;
            fprintf(stderr, "PORT_STACK %s:", b->name);
            for (uint32_t off = 0, shown = 0; off < 4096 && shown < 24; off += 4) {
                uint32_t v = 0;
                if (gm_read(g_mem, esp + off, &v, 4)) break;
                if (v >= 0x00401000u && v < 0x00A2F000u) { fprintf(stderr, " +%X:%08X", off, v); shown++; }
            }
            fprintf(stderr, "\n");
        }
    }
    if (!port_stats_active()) {
        int rc = sb_invoke_inner(b, esp, eax, edx, st0_valid, st0_bits);
        port_act_leave(previous);
        return rc;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = sb_invoke_inner(b, esp, eax, edx, st0_valid, st0_bits);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec));
    port_act_leave(previous);
    if (b) {
        port_bridge_stat(b->name, ns);
        if (!strcmp(b->name, "WaitForSingleObject") && eax) {
            uint32_t caller = 0, timeout = 0;
            gm_read(g_mem, esp, &caller, 4);
            gm_read(g_mem, esp + 8, &timeout, 4);
            port_wait_stat(caller, timeout, *eax == 0x102u, ns);
        }
    }
    return rc;
}
static int sb_invoke_inner(const StaticBridge *b, uint32_t esp, uint32_t *eax, uint32_t *edx,
                           int *st0_valid, uint64_t *st0_bits) {
    if (!b) return GM_UNSUPPORTED;
    uint32_t args[16];
    memset(args, 0, sizeof args);
    uint32_t words = b->argbytes / 4;
    if (words > 16) return GM_UNSUPPORTED;
    for (uint32_t i = 0; i < words; i++)
        if (gm_read(g_mem, esp + 4 + i * 4, &args[i], 4)) return GM_UNCOMMITTED;

    uint32_t result = 0;
    int rc;
    g_bridge_edx_valid = 0;
    switch (b->kind) {
    case SB_CRT: {
        GuestCrt *crt = sb_crt();
        if (!crt) return GM_UNSUPPORTED;
        const GcrtExport *e = (const GcrtExport *)b->entry;
        GcrtReturn ret;
        memset(&ret, 0, sizeof ret);
        rc = e->fn(crt, esp, &ret);
        if (rc) return rc;
        result = ret.eax;
        if (ret.edx_valid && edx) *edx = ret.edx;
        if (ret.st0_valid) {
            /* Push the binary64 onto the modelled x87 stack the same way the
             * lifted `fld` does (runtime/recomp_override/recomp_types.h,
             * fp_push_impl). A11's note describes the recomp32_cpu model's
             * pre-decrement push; the static lift uses the shifting model, and
             * mixing the two would put the value in the wrong slot for the
             * lifted `fstp` that consumes it. */
            double value;
            memcpy(&value, &ret.st0_bits, sizeof value);
            fp_push_impl(g_st, &g_fp_top, value);
            g_st_i64[0] = 0;
            if (st0_valid) *st0_valid = 1;
            if (st0_bits) *st0_bits = ret.st0_bits;
        }
        break;
    }
    case SB_USER32: {
        if (!g_user32) return GM_UNSUPPORTED;
        const GuUser32Export *e = (const GuUser32Export *)b->entry;
        rc = e->fn(g_user32, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_COM: {
        if (!g_com) return GM_UNSUPPORTED;
        const GuComExport *e = (const GuComExport *)b->entry;
        rc = e->fn(g_com, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_OLE: {
        GuestOle *ole = sb_ole();
        if (!ole) return GM_UNSUPPORTED;
        const GuestOleExport *e = (const GuestOleExport *)b->entry;
        rc = e->fn(ole, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_SHELLGDI: {
        if (!g_shellgdi) return GM_UNSUPPORTED;
        const GuestShellGdiExport *e = (const GuestShellGdiExport *)b->entry;
        rc = e->fn(g_shellgdi, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_ADVAPI: {
        if (!g_advapi_extra) return GM_UNSUPPORTED;
        const GuestAdvapiExtraExport *e =
            (const GuestAdvapiExtraExport *)b->entry;
        rc = e->fn(g_advapi_extra, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_WSN: {
        if (!g_winsock_net) return GM_UNSUPPORTED;
        const GuestWinsockNetExport *e = (const GuestWinsockNetExport *)b->entry;
        rc = e->fn(g_winsock_net, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_WINMM: {
        if (!g_winmm) return GM_UNSUPPORTED;
        const GuestWinmmExport *e = (const GuestWinmmExport *)b->entry;
        rc = e->fn(g_winmm, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_DINPUT: {
        if (!g_dinput) return GM_UNSUPPORTED;
        const GuestDinputExport *e = (const GuestDinputExport *)b->entry;
        rc = e->fn(g_dinput, args, &result);
        if (rc) return rc;
        break;
    }
    case SB_XINPUT: {
        if (!g_xinput) return GM_UNSUPPORTED;
        const GuestXinputExport *e = (const GuestXinputExport *)b->entry;
        rc = e->fn(g_xinput, args, &result);
        if (rc) return rc;
        {   /* One line per milestone: the image polls the pad / rumbles it. */
            static int polled, rumbled;
            if (e->fn == gxi_get_state && !polled) {
                polled = 1;
                fprintf(stderr, "PORT_XINPUT first XInputGetState(user %u) -> %u\n", args[0], result);
            } else if (e->fn == gxi_set_state && !rumbled) {
                rumbled = 1;
                fprintf(stderr, "PORT_XINPUT first XInputSetState(user %u) -> %u\n", args[0], result);
            }
        }
        break;
    }
    case SB_KMISC: {
        if (!g_kmisc) return GM_UNSUPPORTED;
        /* LastError is per guest thread, so the module is told which TEB to
         * write before every call. */
        gkm_set_teb(g_kmisc, g_teb);
        const GuestKernelMiscExport *e = (const GuestKernelMiscExport *)b->entry;
        rc = e->fn(g_kmisc, args, &result);
        if (rc) return rc;
        uint32_t high = 0;
        if (gkm_take_edx(g_kmisc, &high)) {
            /* 64-bit return (VerSetConditionMask): EDX:EAX. */
            g_bridge_edx = high;
            g_bridge_edx_valid = 1;
        }
        break;
    }
    default:
        rc = b->fn(args, &result);
        if (rc) return rc;
        break;
    }
    *eax = result;
    if (g_bridge_edx_valid && edx) *edx = g_bridge_edx;
    return GM_OK;
}

void sb_census(SbCensus *out) {
    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < g_table_count; i++) {
        const StaticBridge *b = &g_table[i];
        switch (b->kind) {
        case SB_CRT: out->crt++; break;
        case SB_USER32: out->user32++; break;
        case SB_COM: out->com++; break;
        default: {
            if (!strcmp(b->dll, "advapi32.dll")) out->registry++;
            else if (!strcmp(b->name, "WSACleanup")) out->winsock++;
            else if (!strcmp(b->name, "lstrcmpiW")) out->string_compare++;
            else if (!strcmp(b->name, "OpenProcess") || !strcmp(b->name, "K32EnumProcesses") ||
                     !strcmp(b->name, "K32GetModuleBaseNameW")) out->process_query++;
            else if (strstr(b->name, "SRWLock")) out->srw++;
            else if (strstr(b->name, "Event") || strstr(b->name, "Mutex") ||
                     strstr(b->name, "Semaphore") || strstr(b->name, "WaitFor"))
                out->sync++;
            else if (strstr(b->name, "Thread") || !strcmp(b->name, "Sleep") ||
                     !strcmp(b->name, "SleepEx"))
                out->thread++;
            else if (strstr(b->name, "Time") || strstr(b->name, "Tick"))
                out->time++;
            else if (strstr(b->name, "Virtual"))
                out->vmem++;
            else if (strstr(b->name, "File") || strstr(b->name, "Directory") ||
                     strstr(b->name, "Find") || !strcmp(b->name, "CloseHandle"))
                out->file++;
            else
                out->kernel32++;
            break;
        }
        }
    }
}

/* DirectInput COM method trap (see runtime/guest_dinput.h): returns the stdcall
 * argument bytes to pop, or a negative named stop. */
int sb_dinput_call(uint32_t trap, const uint32_t *args, uint32_t *result, const char **what) {
    static RECOMP_TLS char text[96];
    uint32_t iface = 0, slot = 0;
    if (!g_dinput || gdi_resolve(g_dinput, trap, &iface, &slot)) { if (what) *what = "dinput-bad-trap"; return -1; }
    snprintf(text, sizeof text, "%s::slot%u", gdi_interface_name(iface), slot);
    if (what) *what = text;
    int rc = gdi_call(g_dinput, trap, args, result);
    if (rc) return rc < 0 ? rc : -rc;
    return (int)gdi_method_arg_bytes(iface, slot);
}

GuestDinput *sb_dinput(void) { return g_dinput; }

/* WMI COM method trap (runtime/guest_wbem.h), same protocol. */
int sb_wbem_call(uint32_t trap, const uint32_t *args, uint32_t *result, const char **what) {
    if (!g_wbem) { if (what) *what = "wbem-not-initialised"; return -1; }
    return gwb_call(g_wbem, trap, args, result, what);
}

/* SetEvent from a host thread that is not a guest thread (audio mixer). */
int port_set_guest_event(uint32_t scratch_teb, uint32_t handle) {
    uint32_t result = 0;
    return gso_set_event(g_mem, scratch_teb, handle, &result);
}

/* CloseHandle for runtime pieces that own no handle kind themselves. */
int sb_close_handle_public(uint32_t handle, uint32_t *result) {
    uint32_t args[1] = { handle };
    return sb_close_handle(args, result);
}

/* Gamepad state of the platform -> XInput controller 0 (the original image
 * reads the pad through XInputGetState). PORT_PAD_* bits are in XInput order
 * with A/B/X/Y moved up to 0x1000..0x8000; Android's Y axes point down,
 * XInput's up. */
#include "../android/jni/port_host_api.h"
int sb_gamepad_to_xinput(const port_gamepad_state *state) {
    if (!g_xinput || !state) return GM_UNSUPPORTED;
    uint32_t buttons = (state->dpad & 0xFu) | (state->buttons & 0x3F0u) | ((state->buttons & 0x3C00u) << 2);
    float hx = state->axis[PORT_AXIS_HAT_X], hy = state->axis[PORT_AXIS_HAT_Y];
    if (hy < -0.5f) buttons |= 0x1u; if (hy > 0.5f) buttons |= 0x2u;
    if (hx < -0.5f) buttons |= 0x4u; if (hx > 0.5f) buttons |= 0x8u;
#define SB_STICK(v) ((int32_t)((v) >= 1.0f ? 32767 : ((v) <= -1.0f ? -32768 : (v) * 32767.0f)))
#define SB_TRIGGER(v) ((uint32_t)((v) >= 1.0f ? 255 : ((v) <= 0.0f ? 0 : (v) * 255.0f)))
    return gxi_set_host_state(g_xinput, 0, buttons, SB_TRIGGER(state->axis[PORT_AXIS_LEFT_TRIGGER]),
                              SB_TRIGGER(state->axis[PORT_AXIS_RIGHT_TRIGGER]), SB_STICK(state->axis[PORT_AXIS_LEFT_X]),
                              SB_STICK(-state->axis[PORT_AXIS_LEFT_Y]), SB_STICK(state->axis[PORT_AXIS_RIGHT_X]),
                              SB_STICK(-state->axis[PORT_AXIS_RIGHT_Y]), state->connected != 0 || g_gamepad_native);
}
