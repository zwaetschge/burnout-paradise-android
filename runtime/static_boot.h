#ifndef BURNOUT_STATIC_BOOT_H
#define BURNOUT_STATIC_BOOT_H
#include <stdint.h>
/* Oracle boot environment for the static image (tools/dump_static_boot.py):
 * the initial guest regions the proven startup model maps besides the main
 * image (NTDLL/Kernel32 file views, SharedUserData page, ...) and its module /
 * export registry with the exact guest addresses. Private input; without it
 * the static image runs with runtime-assigned thunk addresses only. */
int static_boot_load(const char *dir, uint8_t *flat);
int static_boot_loaded(void);
/* Region table (data regions only). copy != 0 copies the bytes into flat. */
uint32_t static_boot_region_count(void);
int static_boot_region(uint32_t i, uint32_t *address, uint32_t *size, uint32_t *perms, const uint8_t **bytes);
uint32_t static_boot_module_count(void);
const char *static_boot_module(uint32_t i, uint32_t *handle);
/* Reference count the proven model starts module i with (0 = registered, not loaded). */
uint32_t static_boot_module_refs(uint32_t i);
/* PE file views the proven model maps (NTDLL, Kernel32): base, size, entry point. */
uint32_t static_boot_view_count(void);
void static_boot_view(uint32_t i, uint32_t *base, uint32_t *size, uint32_t *entry);
/* Export address for dll!name (0 when the registry has none). */
uint32_t static_boot_export_address(const char *dll, const char *name);
/* dll!name for a registered export address; returns 0 when found. */
int static_boot_export_at(uint32_t va, const char **dll, const char **name);
#endif
