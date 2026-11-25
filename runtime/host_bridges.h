#ifndef BURNOUT_HOST_BRIDGES_H
#define BURNOUT_HOST_BRIDGES_H
#include <setjmp.h>
#include <stdint.h>
#include "guest_memory.h"
#include "guest_apiset.h"
#include "guest_environment.h"
#include "guest_peb.h"
#include "guest_thread_times.h"
#include "shim_loader.h"

/* Bridge layer for the Win32 API table (runtime/win32_api_table.c).
 *
 * These are the "run_*" entry points the static full build needs: each one
 * consumes an x86 stdcall frame from the guest stack, calls the matching
 * C component, writes the guest result registers and returns the guest.
 *
 * The file is deliberately free of generated slice code: no instructions.c,
 * image_layout.c, module_registry.c, apiset_registry.c or image_views.c. The
 * few facts that only a generated slice knows (module registry, image views,
 * API-set aliases, environment profile, the KUSER clock) arrive through
 * HostBridgeContext, and the guest CPU is reached through four register
 * accessors so the generated cpu.h stays the single owner of that ABI.
 *
 * A host fills the context once (hb_set_context) and then calls the bridges.
 * runtime/startup_host.c does that from its setup(); runtime/static_entry.c
 * can do the same from its own bring-up. No bridge invents a success: every
 * unsupported input returns a named GM_* code after the component said so. */

/* The only CPU fields any bridge reads or writes. The host maps them onto its
 * real CPU struct (generated cpu.h); host_bridges.c never sees that struct. */
enum HbCpuField { HB_CPU_EAX, HB_CPU_ESP, HB_CPU_EIP, HB_CPU_FS_BASE };

typedef struct HostBridgeContext {
    GuestMemory *memory;
    void *cpu;
    uint32_t (*cpu_get)(void *cpu, enum HbCpuField field);
    void (*cpu_set)(void *cpu, enum HbCpuField field, uint32_t value);
    /* 36-byte register snapshot plus packed EFLAGS in word 9, exactly what
     * startup_host.c's state() produced before this split. */
    void (*cpu_state)(void *cpu, uint32_t *out);
    /* Checked guest memory access; a fault longjmps to *fault_return. */
    void (*guest_read)(uint32_t address, void *out, uint32_t size);
    void (*guest_write)(uint32_t address, const void *in, uint32_t size);
    jmp_buf *fault_return;
    /* Generated-slice facts: registered shim modules, image views, API sets. */
    ShimLoader *loader;
    uint32_t image_base, image_size, process_entry;
    const GuestModuleImage *image_views;
    uint32_t image_view_count;
    const GuestApiSet *api_sets;
    uint32_t api_set_count;
    /* Process environment profile (generated environment.c). */
    const GuestEnvironmentEntry *environment;
    uint32_t environment_count;
    /* Shared (KUSER) clock owned by the host: copy-and-advance, then commit. */
    int (*clock_next)(void *system_time, uint64_t *ticks);
    void (*clock_store)(const void *system_time);
} HostBridgeContext;

/* Returns GM_UNSUPPORTED unless memory, cpu, all cpu accessors, both guest
 * memory accessors and the fault buffer are present. The struct is copied. */
int hb_set_context(const HostBridgeContext *context);
const HostBridgeContext *hb_context(void);
/* Drop per-API state. Needs no guest memory; call before the memory is freed. */
void hb_reset_state(void);
/* Create per-API state on the context's memory. Non-zero means abort the run. */
int hb_initialize_state(void);

/* --- bridges (defined only when their WITH_* component is compiled in) --- */
int run_main_file_name(uint32_t *out);
int run_pseudo_handle(uint32_t thread, uint32_t *out);
int run_identity(uint32_t thread, uint32_t *out);
int run_filetime_value(uint64_t ticks, uint32_t *out);
int run_filetime(uint32_t *out);
int run_processor_feature(uint32_t *out);
int run_critical_initialize(uint32_t *out);
int run_winsock_startup(uint32_t *out);
uint32_t winsock_references(void);
int run_thread_times_value(const GuestThreadTimes *times, uint32_t *out);
int run_thread_times(uint32_t *out);
int initialize_pointer_cookie(uint32_t cookie);
int initialize_random_pointer_cookie(void);
int run_pointer_codec(uint32_t which, uint32_t *out);
int run_environment_copy(uint32_t which, uint32_t *out);
uint32_t environment_copy_blocks(uint32_t *addresses, uint32_t capacity);
int run_condition_initialize(uint32_t *out);
int run_srw_initialize(uint32_t *out);
int run_slist_initialize(uint32_t *out);
int initialize_case_maps(const uint8_t *raw, uint32_t size);
int run_case_map(uint32_t *out);
int initialize_character_types(const uint8_t *raw, uint32_t size);
int run_string_type(uint32_t *out);
int initialize_locale(uint32_t ansi);
int install_code_page(const uint8_t *bytes, uint32_t size);
int run_wide_to_mb(uint32_t *out);
int run_mb_to_wide(uint32_t *out);
int run_code_page_info(uint32_t *out);
int run_valid_code_page(uint32_t *out);
int run_ansi_code_page(uint32_t *out);
int run_std_handle(uint32_t *out);
int run_startup_info(uint32_t *out);
int run_critical(uint32_t which, uint32_t *out);
int run_fls(uint32_t which, uint32_t *out);
void fls_snapshot(uint32_t *out);
int initialize_process_heap(void);
uint32_t process_heap_regions(uint32_t *triples, uint32_t capacity);
int run_process_heap(uint32_t which, uint32_t *out);
int run_virtual_protect(uint32_t *out);
int run_loader(uint32_t which, uint32_t *out);
int run_process_parameters(uint32_t which, uint32_t *out);
int run_tls(uint32_t which, uint32_t *out);
void tls_snapshot(uint32_t *values);
int run_clock(uint32_t which, uint64_t captured_ns, uint32_t *out);
int initialize_environment(void *backing);
int run_environment(uint32_t which, uint32_t *out);
int run_local(uint32_t which, uint32_t *out);
uint32_t local_regions(uint32_t *pairs, uint32_t capacity);

#endif
