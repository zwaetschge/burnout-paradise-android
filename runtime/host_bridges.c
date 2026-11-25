/* Win32 API bridges, independent of the generated slice harness.
 *
 * Every run_* entry point below was moved verbatim out of
 * runtime/startup_host.c: same stdcall frame handling, same component calls,
 * same guest result registers, same named stop on unsupported input. The only
 * change is how the guest CPU and the generated-slice facts are reached:
 * through HostBridgeContext instead of the harness's file-scope statics.
 *
 * This translation unit must build on its own (no instructions.c,
 * image_layout.c, module_registry.c, apiset_registry.c, image_views.c), which
 * is what the static full build links against runtime/win32_api_table.c. */
#include "host_bridges.h"
#include <stdlib.h>
#include <string.h>
#include "guest_case_map.h"
#include "guest_character_type.h"
#include "guest_clock.h"
#include "guest_commandline.h"
#include "guest_condition.h"
#include "guest_critical.h"
#include "guest_environment_copies.h"
#include "guest_fls.h"
#include "guest_locale.h"
#include "guest_pointer_codec.h"
#include "guest_process_heap.h"
#include "guest_shared_data.h"
#include "guest_slist.h"
#include "guest_startup_info.h"
#include "guest_system_time.h"
#include "guest_tls.h"
#include "guest_winsock.h"
#include "local_heap.h"
#include "shim_loader_wide.h"

static HostBridgeContext hb_storage;
static const HostBridgeContext *hb;

int hb_set_context(const HostBridgeContext *context) {
    if (!context || !context->memory || !context->cpu || !context->cpu_get ||
        !context->cpu_set || !context->cpu_state || !context->guest_read ||
        !context->guest_write || !context->fault_return)
        return GM_UNSUPPORTED;
    hb_storage = *context;
    hb = &hb_storage;
    return GM_OK;
}

const HostBridgeContext *hb_context(void) { return hb; }

/* Enter a bridge: no context is a named stop, a guest memory fault restores
 * the CPU snapshot and returns -2 exactly like the old harness did. */
#define HB_BEGIN(out)                                                     \
    do {                                                                  \
        if (!hb) return GM_UNSUPPORTED;                                   \
        if (setjmp(*hb->fault_return)) {                                  \
            hb_state(out);                                                \
            return -2;                                                    \
        }                                                                 \
    } while (0)

/* A build that compiles a component out must not warn about an accessor that
 * only that component used; the real build defines every WITH_* switch. */
#if defined(__GNUC__) || defined(__clang__)
#define HB_UNUSED __attribute__((unused))
#else
#define HB_UNUSED
#endif
static HB_UNUSED uint32_t hb_cpu(enum HbCpuField field) { return hb->cpu_get(hb->cpu, field); }
static HB_UNUSED void hb_set_cpu(enum HbCpuField field, uint32_t value) { hb->cpu_set(hb->cpu, field, value); }
static HB_UNUSED void hb_state(uint32_t *out) { hb->cpu_state(hb->cpu, out); }
static HB_UNUSED uint32_t hb_rd32(uint32_t address) {
    uint32_t value = 0;
    hb->guest_read(address, &value, 4);
    return value;
}
static HB_UNUSED void hb_wr32(uint32_t address, uint32_t value) { hb->guest_write(address, &value, 4); }
static HB_UNUSED uint32_t hb_teb(void) { return hb_cpu(HB_CPU_FS_BASE); }

/* ---- per-API state (one guest thread, one process) ---- */
#ifdef WITH_GUEST_WINSOCK
static GuestWinsock winsock;
#endif
#ifdef WITH_THREAD_TIMES
static GuestThreadClock thread_clock;
#endif
#ifdef WITH_POINTER_CODEC
static uint32_t pointer_cookie;
#endif
#ifdef WITH_ENVIRONMENT_COPIES
static GuestEnvironmentCopies *environment_copies;
#endif
#ifdef WITH_GUEST_CASE_MAP
static GuestCaseMaps *case_maps;
#endif
#ifdef WITH_GUEST_CHARACTER_TYPES
static GuestCharacterTypes *character_types;
#endif
#ifdef WITH_GUEST_LOCALE
static GuestLocale process_locale;
#endif
#ifdef WITH_GUEST_FLS
static GuestFls *default_fls;
#endif
#ifdef WITH_PROCESS_HEAP
static GuestProcessHeap *process_heap;
#endif
#ifdef WITH_LOCAL_HEAP
static GuestLocalHeap *local_heap;
#endif
#ifdef WITH_GUEST_TLS
static GuestTlsProcess *tls_process;
static GuestTlsThread *tls_thread;
#endif

void hb_reset_state(void) {
#ifdef WITH_GUEST_WINSOCK
    memset(&winsock, 0, sizeof(winsock));
#endif
#ifdef WITH_THREAD_TIMES
    if (gtt_start(&thread_clock)) abort();
#endif
#ifdef WITH_POINTER_CODEC
    pointer_cookie = 0;
#endif
#ifdef WITH_ENVIRONMENT_COPIES
    gec_destroy(environment_copies);
    environment_copies = NULL;
#endif
#ifdef WITH_GUEST_CASE_MAP
    gcm_destroy(case_maps);
    case_maps = NULL;
#endif
#ifdef WITH_GUEST_CHARACTER_TYPES
    gct_destroy(character_types);
    character_types = NULL;
#endif
#ifdef WITH_GUEST_LOCALE
    memset(&process_locale, 0, sizeof(process_locale));
#endif
#ifdef WITH_GUEST_FLS
    gfl_destroy(default_fls);
    default_fls = gfl_create();
    if (!default_fls) abort();
#endif
#ifdef WITH_LOCAL_HEAP
    lh_destroy(local_heap);
    local_heap = NULL;
#endif
#ifdef WITH_PROCESS_HEAP
    gph_destroy(process_heap);
    process_heap = NULL;
#endif
}

int hb_initialize_state(void) {
    if (!hb) return GM_UNSUPPORTED;
#ifdef WITH_PROCESS_HEAP
    if (!(process_heap = gph_create(hb->memory))) return GM_ALLOCATION;
#ifdef WITH_ENVIRONMENT_COPIES
    if (!(environment_copies = gec_create(hb->memory, process_heap, 0x74001000u)))
        return GM_ALLOCATION;
#endif
#endif
#ifdef WITH_LOCAL_HEAP
    if (!(local_heap = lh_create(hb->memory))) return GM_ALLOCATION;
#endif
#ifdef WITH_GUEST_TLS
    gt_process_destroy(tls_process);
    tls_process = gt_process_create();
    if (!tls_process || !(tls_thread = gt_thread_create(tls_process))) return GM_ALLOCATION;
#endif
#ifdef WITH_GUEST_FLS
    /* A host that never called hb_reset_state() (the flat runtime) still needs
     * the default fiber's FLS table; without it FlsAlloc reports
     * FLS_OUT_OF_INDEXES and the guest CRT refuses to initialise. */
    if (!default_fls && !(default_fls = gfl_create())) return GM_ALLOCATION;
#endif
    return GM_OK;
}

#if defined(WITH_GUEST_PEB) || defined(WITH_GUEST_IDENTITY)
int run_main_file_name(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    int rc = gp_main_file_name_a(hb->memory, hb_rd32(hb_teb() + 0x30), hb_rd32(sp + 4),
                                 hb_rd32(sp + 8), hb_rd32(sp + 12), &value, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 16);
    hb_state(out);
    return GM_OK;
}
int run_pseudo_handle(uint32_t thread, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t value;
    int rc = gp_current_pseudo(thread, &value);
    if (rc) { hb_state(out); return rc; }
    uint32_t ret = hb_rd32(hb_cpu(HB_CPU_ESP));
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, hb_cpu(HB_CPU_ESP) + 4);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_state(out);
    return GM_OK;
}
int run_identity(uint32_t thread, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t value;
    int rc = gp_current_identity(hb->memory, hb_teb(), thread, &value);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, hb_rd32(hb_cpu(HB_CPU_ESP)));
    hb_set_cpu(HB_CPU_ESP, hb_cpu(HB_CPU_ESP) + 4);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_state(out);
    return GM_OK;
}
#endif

#if defined(WITH_SHARED_USER_DATA) || defined(WITH_GUEST_FILETIME)
int run_filetime_value(uint64_t ticks, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), output = hb_rd32(sp + 4);
    int rc = gst_filetime_value(hb->memory, output, ticks);
    if (rc) { hb_state(out); return rc; }
    /* VOID API: preserve registers/flags, consume one stdcall argument. */
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_SHARED_USER_DATA
int run_filetime(uint32_t *out) {
    if (!hb) return GM_UNSUPPORTED;
    if (!hb->clock_next || !hb->clock_store) { hb_state(out); return GM_UNSUPPORTED; }
    GuestSystemTime next;
    uint64_t ticks;
    int rc = hb->clock_next(&next, &ticks);
    if (rc) { hb_state(out); return rc; }
    rc = run_filetime_value(ticks, out);
    if (!rc) hb->clock_store(&next);
    return rc;
}
#endif

#if defined(WITH_SHARED_USER_DATA) || defined(WITH_GUEST_FEATURES)
int run_processor_feature(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), feature = hb_rd32(sp + 4), value;
    int rc = gs_processor_feature(hb->memory, feature, &value);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_CRITICAL
int run_critical_initialize(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp);
    int rc = gcs_initialize(hb->memory, hb_rd32(sp + 4), hb_rd32(sp + 8), hb_rd32(sp + 12),
                            hb_rd32(0x75000064u));
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, 1);
    hb_set_cpu(HB_CPU_ESP, sp + 16);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_WINSOCK
int run_winsock_startup(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value;
    int rc = gws_startup(hb->memory, &winsock, hb_rd32(sp + 4), hb_rd32(sp + 8), &value);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 12);
    hb_state(out);
    return GM_OK;
}
uint32_t winsock_references(void) { return winsock.references; }
#endif

#ifdef WITH_THREAD_TIMES
int run_thread_times_value(const GuestThreadTimes *times, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), outputs[4];
    for (unsigned i = 0; i < 4; i++) outputs[i] = hb_rd32(sp + 8 + i * 4);
    int rc = gtt_write(hb->memory, hb_rd32(sp + 4), outputs, times);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, 1);
    hb_set_cpu(HB_CPU_ESP, sp + 24);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_state(out);
    return GM_OK;
}
int run_thread_times(uint32_t *out) {
    GuestThreadTimes times;
    int rc = gtt_sample(&thread_clock, &times);
    if (rc) { hb_state(out); return rc; }
    return run_thread_times_value(&times, out);
}
#endif

#ifdef WITH_POINTER_CODEC
int initialize_pointer_cookie(uint32_t cookie) {
    if (!cookie || pointer_cookie) return GM_UNSUPPORTED;
    pointer_cookie = cookie;
    return GM_OK;
}
int initialize_random_pointer_cookie(void) {
    if (pointer_cookie) return GM_UNSUPPORTED;
    return gpc_random_cookie(&pointer_cookie);
}
int run_pointer_codec(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value;
    int rc = gpc_transform(pointer_cookie, which, hb_rd32(sp + 4), &value);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_ENVIRONMENT_COPIES
int run_environment_copy(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value = 0, cleanup = 4;
    int rc;
    if (which == 1) {
        uint32_t peb = hb_rd32(hb_teb() + 0x30), parameters = hb_rd32(peb + 0x10);
        if (hb_rd32(parameters + 0x48) != 0x74001000u) { hb_state(out); return GM_UNSUPPORTED; }
        rc = gec_get_w(environment_copies, &value);
    } else if (which == 2) {
        rc = gec_free_w(environment_copies, hb_rd32(sp + 4));
        value = 1;
        cleanup = 8;
    } else rc = GM_UNSUPPORTED;
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_state(out);
    return GM_OK;
}
uint32_t environment_copy_blocks(uint32_t *addresses, uint32_t capacity) {
    return gec_blocks(environment_copies, addresses, capacity);
}
#endif

#ifdef WITH_GUEST_CONDITION
int run_condition_initialize(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp);
    int rc = gcv_initialize(hb->memory, hb_rd32(sp + 4));
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
/* PE32 SRW and condition-variable initial states are the same zero pointer.
 * Only initialization is shared; acquire/release/wait/wake are unsupported. */
int run_srw_initialize(uint32_t *out) { return run_condition_initialize(out); }
#endif

#ifdef WITH_GUEST_SLIST
int run_slist_initialize(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp);
    int rc = gsl_initialize(hb->memory, hb_rd32(sp + 4));
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_CASE_MAP
int initialize_case_maps(const uint8_t *raw, uint32_t size) {
    GuestCaseMaps *next = NULL;
    int rc = gcm_create(raw, size, &next);
    if (!rc) { gcm_destroy(case_maps); case_maps = next; }
    return rc;
}
int run_case_map(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    if (hb_rd32(sp + 4) || hb_rd32(sp + 28) || hb_rd32(sp + 32) || hb_rd32(sp + 36)) {
        hb_state(out);
        return GM_UNSUPPORTED;
    }
    int rc = gcm_string(case_maps, hb->memory, hb_rd32(sp + 8), hb_rd32(sp + 12),
                        (int32_t)hb_rd32(sp + 16), hb_rd32(sp + 20),
                        (int32_t)hb_rd32(sp + 24), &value, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 40);
    hb_state(out);
    return GM_OK;
}

/* LCMapStringW(LCID, dwMapFlags, lpSrcStr, cchSrc, lpDestStr, cchDest): the
 * original's ANSI CRT uses this older 6-arg form (the Remastered used
 * LCMapStringEx). Same filesystem case mapping (gcm_string); only the default
 * locales are modelled, any other LCID stops instead of guessing. */
int run_case_map_w(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    uint32_t lcid = hb_rd32(sp + 4);
    if (lcid != 0 && lcid != 0x400 && lcid != 0x800 && lcid != 0x409) { hb_state(out); return GM_UNSUPPORTED; }
    int rc = gcm_string(case_maps, hb->memory, hb_rd32(sp + 8), hb_rd32(sp + 12),
                        (int32_t)hb_rd32(sp + 16), hb_rd32(sp + 20),
                        (int32_t)hb_rd32(sp + 24), &value, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 28);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_CHARACTER_TYPES
int initialize_character_types(const uint8_t *raw, uint32_t size) {
    GuestCharacterTypes *next = NULL;
    int rc = gct_create(raw, size, &next);
    if (!rc) { gct_destroy(character_types); character_types = next; }
    return rc;
}
int run_string_type(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    int rc = gct_string_type(character_types, hb->memory, hb_rd32(sp + 4), hb_rd32(sp + 8),
                             (int32_t)hb_rd32(sp + 12), hb_rd32(sp + 16), &value, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 20);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_LOCALE
int initialize_locale(uint32_t ansi) { return gl_initialize(&process_locale, ansi); }
int install_code_page(const uint8_t *bytes, uint32_t size) {
    return gl_install_cp1252(&process_locale, bytes, size);
}
int run_wide_to_mb(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    uint32_t def_ptr = hb_rd32(sp + 28), used_ptr = hb_rd32(sp + 32);
    int default_char = -1;
    if (def_ptr) { uint8_t dc = 0; if (gm_read(hb->memory, def_ptr, &dc, 1)) { hb_state(out); return GM_UNCOMMITTED; } default_char = dc; }
    uint32_t used_default = 0;
    int rc = gl_wide_to_mb(&process_locale, hb->memory, hb_rd32(sp + 4), hb_rd32(sp + 8),
                           hb_rd32(sp + 12), (int32_t)hb_rd32(sp + 16), hb_rd32(sp + 20),
                           (int32_t)hb_rd32(sp + 24), default_char, &used_default, &value, &error);
    if (!rc && used_ptr) { uint8_t b4[4] = {(uint8_t)used_default,0,0,0}; if (gm_write(hb->memory, used_ptr, b4, 4)) { hb_state(out); return GM_UNCOMMITTED; } }
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 36);
    hb_state(out);
    return GM_OK;
}
int run_mb_to_wide(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    int rc = gl_mb_to_wide(&process_locale, hb->memory, hb_rd32(sp + 4), hb_rd32(sp + 8),
                           hb_rd32(sp + 12), (int32_t)hb_rd32(sp + 16), hb_rd32(sp + 20),
                           (int32_t)hb_rd32(sp + 24), &value, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 28);
    hb_state(out);
    return GM_OK;
}
int run_code_page_info(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value, error;
    int rc = gl_cp_info(&process_locale, hb->memory, hb_rd32(sp + 4), hb_rd32(sp + 8),
                        &value, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 12);
    hb_state(out);
    return GM_OK;
}
int run_valid_code_page(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value;
    int rc = gl_valid_code_page(&process_locale, hb_rd32(sp + 4), &value);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
int run_ansi_code_page(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), value;
    int rc = gl_ansi_code_page(&process_locale, &value);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EAX, value);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 4);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_STARTUP_INFO
int run_std_handle(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), result, error;
    int rc = gsi_std_handle(hb->memory, hb_rd32(hb_teb() + 0x30), hb_rd32(sp + 4), &result, &error);
    if (rc) { hb_state(out); return rc; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
int run_startup_info(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), output = hb_rd32(sp + 4);
    int rc = gsi_get(hb->memory, hb_rd32(hb_teb() + 0x30), output);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_CRITICAL
int run_critical(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), section = hb_rd32(sp + 4),
             thread = hb_rd32(hb_teb() + 0x24), entered = 0;
    int rc;
    if (which == 0 || which == 2) rc = gcs_enter(hb->memory, section, thread, which == 2, &entered);
    else if (which == 1) rc = gcs_leave(hb->memory, section, thread);
    else rc = GM_UNSUPPORTED;
    if (rc) { hb_state(out); return rc; }
    if (which == 2) hb_set_cpu(HB_CPU_EAX, entered); /* Enter/Leave are VOID; preserve EAX. */
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_state(out);
    return GM_OK;
}
#endif

#ifdef WITH_GUEST_FLS
int run_fls(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), arg = hb_rd32(sp + 4), result = 0, error = 0,
             ret = hb_rd32(sp), cleanup = 8;
    if (which == 1) {
        if (arg) {
            int rc = gm_check(hb->memory, arg, 1, GM_EXECUTE);
            if (rc) { hb_state(out); return rc; }
        }
        error = gfl_alloc(default_fls, arg, &result);
        if (error) result = UINT32_MAX;
    } else if (which == 2) {
        error = gfl_get(default_fls, arg, &result);
        if (!error) hb_wr32(hb_teb() + 0x34, 0);
    } else if (which == 3) {
        error = gfl_set(default_fls, arg, hb_rd32(sp + 8));
        cleanup = 12;
        result = !error;
    } else if (which == 4) {
        error = gfl_free(default_fls, arg);
        result = !error;
    } else { hb_state(out); return GM_UNSUPPORTED; }
    if (error == GFL_CALLBACK_REQUIRED) { hb_state(out); return GM_UNSUPPORTED; }
    if (error) hb_wr32(hb_teb() + 0x34, error);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_state(out);
    return GM_OK;
}
void fls_snapshot(uint32_t *out) { gfl_snapshot(default_fls, out); }
#endif

#ifdef WITH_PROCESS_HEAP
int initialize_process_heap(void) { return gph_initialize(process_heap, 0x75000000u); }
uint32_t process_heap_regions(uint32_t *triples, uint32_t capacity) {
    return gph_regions(process_heap, triples, capacity);
}
int run_process_heap(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), ret = hb_rd32(sp), result = 0, cleanup = 4;
    int rc;
    if (which == 0) {
        rc = gph_current(process_heap, hb_rd32(hb_teb() + 0x30), &result);
    } else if (which == 1) {
        rc = gph_alloc(process_heap, hb_rd32(sp + 4), hb_rd32(sp + 8), hb_rd32(sp + 12), &result);
        cleanup = 16;
    } else if (which == 2) {
        rc = gph_free(process_heap, hb_rd32(sp + 4), hb_rd32(sp + 8), hb_rd32(sp + 12));
        result = 1;
        cleanup = 16;
    } else if (which == 3) {
        rc = gph_size(process_heap, hb_rd32(sp + 4), hb_rd32(sp + 8), hb_rd32(sp + 12), &result);
        cleanup = 16;
    } else if (which == 4) {
        rc = gph_realloc(process_heap, hb_rd32(sp + 4), hb_rd32(sp + 8), hb_rd32(sp + 12),
                         hb_rd32(sp + 16), &result);
        cleanup = 20;
    } else rc = GM_UNSUPPORTED;
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, ret);
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_state(out);
    return GM_OK;
}
#endif

int run_virtual_protect(uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP);
    uint32_t address = hb_rd32(sp + 4), size = hb_rd32(sp + 8), protection = hb_rd32(sp + 12),
             old = hb_rd32(sp + 16);
    int rc = gm_virtual_protect(hb->memory, address, size, protection, old);
    if (rc) { hb_state(out); return rc; } /* unsupported/error paths remain explicit */
    hb_set_cpu(HB_CPU_EIP, hb_rd32(sp));
    hb_set_cpu(HB_CPU_ESP, sp + 20);
    hb_set_cpu(HB_CPU_EAX, 1);
    hb_state(out);
    return 0;
}

#ifdef WITH_SHIM_LOADER
static int hb_read_string(uint32_t address, char *out, size_t capacity) {
    for (size_t i = 0; i < capacity; i++) {
        uint8_t byte = 0;
        hb->guest_read(address + (uint32_t)i, &byte, 1);
        out[i] = (char)byte;
        if (!out[i]) return 0;
        if ((unsigned char)out[i] > 127) return 1;
    }
    return 1;
}
int run_loader(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), result = 0, cleanup = 0;
    int rc = 1;
    char text[1024];
    if (which == 1) {
        if (!hb_read_string(hb_rd32(sp + 4), text, sizeof(text)))
            rc = sl_load(hb->loader, text, &result);
        cleanup = 8;
    } else if (which == 2) {
        uint32_t handle = hb_rd32(sp + 4), pointer = hb_rd32(sp + 8);
        if (pointer <= 0xffff) rc = sl_symbol(hb->loader, handle, NULL, pointer, &result);
        else if (!hb_read_string(pointer, text, sizeof(text)))
            rc = sl_symbol(hb->loader, handle, text, 0, &result);
        cleanup = 12;
#ifdef WITH_GUEST_APISETS
    } else if (which == 3) {
        rc = sl_load_exw(hb->loader, hb->memory, hb->api_sets, hb->api_set_count,
                         hb_rd32(sp + 4), hb_rd32(sp + 8), hb_rd32(sp + 12), &result);
        cleanup = 16;
    } else if (which == 4) {
        rc = sl_get_module_w(hb->loader, hb->memory, hb_rd32(sp + 4), "BurnoutPR.exe",
                             hb->image_base, &result);
        cleanup = 8;
        if (!rc && !result) hb_wr32(hb_teb() + 0x34, 126);
#endif
    }
    if (rc) { hb_state(out); return rc; }
#ifdef WITH_GUEST_PEB
    if ((which == 1 || which == 3) &&
        (rc = gp_sync_modules(hb->memory, hb->image_base, hb->image_size, hb->process_entry,
                              hb->loader))) {
        hb_state(out);
        return rc;
    }
#ifdef WITH_REFERENCE_VIEWS
    if ((which == 1 || which == 3) &&
        (rc = gp_sync_image_views(hb->memory, hb->loader, hb->image_views,
                                  hb->image_view_count))) {
        hb_state(out);
        return rc;
    }
#endif
#endif
    hb_set_cpu(HB_CPU_EIP, hb_rd32(sp));
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_state(out);
    return 0;
}
#endif

#ifdef WITH_PROCESS_PARAMETERS
int run_process_parameters(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    if (which != 1 && which != 2) return 1;
    hb_set_cpu(HB_CPU_EAX, which == 1 ? 0x74000800u : 0x74000000u);
    hb_set_cpu(HB_CPU_EIP, hb_rd32(hb_cpu(HB_CPU_ESP)));
    hb_set_cpu(HB_CPU_ESP, hb_cpu(HB_CPU_ESP) + 4);
    hb_state(out);
    return 0;
}
#endif

#ifdef WITH_GUEST_TLS
int run_tls(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), result = 0, cleanup = 4;
    gt_set_error(tls_thread, hb_rd32(hb_teb() + 0x34));
    if (which == 1) result = gt_alloc(tls_thread);
    else if (which == 2) { result = gt_free(tls_thread, hb_rd32(sp + 4)); cleanup = 8; }
    else if (which == 3) { result = gt_get(tls_thread, hb_rd32(sp + 4)); cleanup = 8; }
    else if (which == 4) { result = gt_set(tls_thread, hb_rd32(sp + 4), hb_rd32(sp + 8)); cleanup = 12; }
    else return 1;
    uint32_t values[GT_SLOTS];
    gt_snapshot(tls_thread, values);
    for (uint32_t i = 0; i < 64; i++) hb_wr32(hb_teb() + 0xe10 + 4 * i, values[i]);
    hb_wr32(hb_teb() + 0x34, gt_error(tls_thread));
    hb_set_cpu(HB_CPU_EIP, hb_rd32(sp));
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_state(out);
    return 0;
}
void tls_snapshot(uint32_t *values) { gt_snapshot(tls_thread, values); }
#endif

#ifdef WITH_GUEST_CLOCK
int run_clock(uint32_t which, uint64_t captured_ns, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), pointer = hb_rd32(sp + 4);
    int rc = GM_UNSUPPORTED;
    if (which == 1) rc = gc_frequency(hb->memory, pointer);
    else if (which == 2)
        rc = captured_ns == UINT64_MAX ? gc_counter(hb->memory, pointer)
                                       : gc_counter_value(hb->memory, pointer, captured_ns);
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, hb_rd32(sp));
    hb_set_cpu(HB_CPU_ESP, sp + 8);
    hb_set_cpu(HB_CPU_EAX, 1);
    hb_state(out);
    return 0;
}
#endif

#ifdef WITH_GUEST_ENVIRONMENT
int initialize_environment(void *backing) {
    static uint8_t immutable[512];
    uint32_t written = 0;
    if (!hb) return GM_UNSUPPORTED;
    memset(immutable, 255, sizeof(immutable));
    int rc = ge_serialize_block(hb->environment, hb->environment_count, backing, GE_BLOCK_SIZE,
                                &written);
    return rc ? rc : gm_limit_readable_bytes(hb->memory, GE_BLOCK_BASE, immutable);
}
int run_environment(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), result = hb_cpu(HB_CPU_EAX), cleanup = 0;
    int rc = GM_OK;
    if (which == 1) {
        rc = ge_get_a(hb->memory, hb->environment, hb->environment_count, hb_rd32(sp + 4),
                      hb_rd32(sp + 8), hb_rd32(sp + 12), hb_teb() + 0x34, &result);
        cleanup = 16;
    } else if (which == 2) {
        result = hb_rd32(hb_teb() + 0x34);
        cleanup = 4;
    } else if (which == 3) {
        hb_wr32(hb_teb() + 0x34, hb_rd32(sp + 4));
        cleanup = 8;
    } else rc = GM_UNSUPPORTED;
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, hb_rd32(sp));
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_state(out);
    return 0;
}
#endif

#ifdef WITH_LOCAL_HEAP
int run_local(uint32_t which, uint32_t *out) {
    HB_BEGIN(out);
    uint32_t sp = hb_cpu(HB_CPU_ESP), result = 0, cleanup = 0;
    int rc = 1;
    if (which == 1) {
        rc = gc_argv(hb->memory, local_heap, hb_rd32(sp + 4), hb_rd32(sp + 8), 0x74000000, &result);
        cleanup = 12;
    } else if (which == 2) {
        rc = lh_free(local_heap, hb_rd32(sp + 4));
        cleanup = 8;
    }
    if (rc) { hb_state(out); return rc; }
    hb_set_cpu(HB_CPU_EIP, hb_rd32(sp));
    hb_set_cpu(HB_CPU_ESP, sp + cleanup);
    hb_set_cpu(HB_CPU_EAX, result);
    hb_state(out);
    return 0;
}
uint32_t local_regions(uint32_t *pairs, uint32_t capacity) {
    return lh_regions(local_heap, pairs, capacity);
}
#endif
