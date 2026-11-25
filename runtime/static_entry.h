#ifndef BURNOUT_STATIC_ENTRY_H
#define BURNOUT_STATIC_ENTRY_H
#include <stdint.h>

#include "guest_process_heap.h"

/* A10 -- process entry for the full static lift (port/upstream/pcrecomp
 * recomp32 model, tools/static_full_lift.py output).
 *
 * port_entry() brings up one guest process and runs the translated entry
 * point (0x08281000, .text1). It is the only symbol an Android/host front end
 * needs; everything else is private to runtime/static_entry.c.
 *
 *   game_root : host directory that guest C:\BurnoutPR maps to (read-only
 *               game data). The recovered EXE is read from
 *               <game_root>/BurnoutPR.exe.
 *   user_root : host directory that the guest user profile maps to
 *               (writable: saves, config).
 *
 * Return value: 0 only when the translated entry point returns normally.
 * Anything the port cannot do is a *named* trap: it prints one
 * "PORT_TRAP {json}" line on stderr and calls exit(70). There is no silent
 * zero stub and no fabricated Win32 success anywhere on this path.
 *
 * The guest address space is one flat 4 GiB host reservation with
 * g_mem_base = reservation base, so the lifted code's MEM32(va) macros address
 * guest memory directly (see port/build/agents/a10-report.md, section
 * "Speichermodell"). */
int port_entry(const char *game_root, const char *user_root);

/* Same, plus an optional host path to the private c_1252.nls bytes. When NULL
 * the ANSI (CP1252) entry points stop instead of guessing bytes. */
int port_entry_ex(const char *game_root, const char *user_root,
                  const char *nls_path);

/* Bring-up helpers used by runtime/static_host_main.c (never by the game). */
int port_setup(const char *game_root, const char *user_root, const char *nls_path);
int port_call_va(uint32_t va);
int port_poke(uint32_t va, const void *bytes, uint32_t size);
int port_peek(uint32_t va, void *bytes, uint32_t size);
int port_selftest_import(const char *dll, const char *name, const uint32_t *args,
                         uint32_t argcount, uint32_t *eax_out, uint32_t *esp_out);

/* Host-driver diagnostics; valid after port_entry_ex() returned or trapped. */
uint32_t port_guest_eip(void);
uint32_t port_guest_esp(void);
uint32_t port_guest_func(void);
/* Number of IAT slots that got a real C bridge (vs. a named stop). */
uint32_t port_bound_imports(void);
uint32_t port_total_imports(void);
/* A14: size of the merged bridge table (every export list of the runtime). */
uint32_t port_bridge_count(void);
/* The flat 4 GiB guest reservation (g_mem_base == this pointer). */
uint8_t *port_flat_base(void);
/* Live guest threads (runtime/static_threads.c). */
uint32_t port_guest_thread_count(void);
/* Export-resolution policy for GetProcAddress().
 *
 * On (the default) an export the runtime does not provide resolves to a real
 * thunk whose body is a named stop: the guest gets a non-NULL function pointer
 * -- as on Windows, where the export exists -- and the port stops with
 * GUEST_API_UNIMPLEMENTED dll!name at the moment the guest *calls* it. That is
 * where the missing semantics actually matter, and it lets initialisation that
 * merely resolves a function table (the .srdata delay-load resolver) finish.
 * Nothing is executed and nothing succeeds: the thunk only reports.
 *
 * Off (--strict-export-resolution) GetProcAddress stops at resolution time. */
void port_set_resolve_unknown_exports(int on);
/* One PORT_GPA line per dynamic resolution (diagnostic). */
void port_set_trace_exports(int on);
/* Test/diagnostic: attach the calling host thread to the guest process with its
 * own guest stack/TEB/TLS vector (all RECOMP_TLS), so a bridge can be driven
 * from a second host thread. Never used by the game path. */
int port_thread_attach(uint32_t esp, uint32_t teb, uint32_t tls_vector);
/* Dump every declared import and whether it is bound, for the wiring audit. */
void port_dump_imports(void);
/* A translated body that is just `ret` / `xor eax,eax; ret` (0 when none). */
uint32_t port_find_trivial_function(void);
/* Live blocks of the flat process heap (runtime/static_heap.c). */
uint32_t gph_live_blocks(GuestProcessHeap *heap);

#endif
