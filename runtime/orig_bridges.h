#ifndef BURNOUT_ORIG_BRIDGES_H
#define BURNOUT_ORIG_BRIDGES_H
#include <stdint.h>
/* Plain stdcall bridges added for the original (2009) image; appended to the
 * merged bridge table by sb_build() (runtime/static_bridges.c). */
typedef int (*OrigBridgeFn)(const uint32_t *a, uint32_t *result);
typedef struct { const char *dll, *name; uint16_t argbytes; OrigBridgeFn fn; } OrigBridge;
extern const OrigBridge orig_bridges[];
extern const uint32_t orig_bridge_count;
extern const OrigBridge orig_winsock_bridges[];   /* runtime/orig_winsock.c */
extern const uint32_t orig_winsock_bridge_count;
/* Exports a feature probe (GetProcAddress) is told do not exist on this
 * platform -- the answer Windows itself gives where the feature is missing.
 * Each entry is a documented decision, not a guess. */
int orig_export_absent(const char *dll, const char *name);
/* Installs the default sort-key table of sortdefault.nls for CompareStringW. */
#include <stddef.h>
void ob_install_sort_keys(const uint8_t *nls, size_t size);
#endif
