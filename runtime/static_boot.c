#include "static_boot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { uint32_t address, size, perms; const uint8_t *bytes; } BootRegion;
typedef struct { uint32_t handle; char name[64]; uint32_t refs; } BootModule;
typedef struct { uint32_t module, target; char name[112]; } BootExport;

static BootRegion g_regions[128]; static uint32_t g_region_count;
static BootModule g_modules[64]; static uint32_t g_module_count;
static BootExport *g_exports; static uint32_t g_export_count, g_export_cap;
static uint32_t *g_by_va; static uint32_t g_by_va_cap;     /* open addressing: index+1 */
static uint8_t *g_blob; static int g_loaded;
static uint32_t g_views[8][3]; static uint32_t g_view_count;   /* base, size, entry */

static void by_va_insert(uint32_t idx) {
    uint32_t h = (g_exports[idx].target * 2654435761u) & (g_by_va_cap - 1);
    while (g_by_va[h]) {
        if (g_exports[g_by_va[h] - 1].target == g_exports[idx].target) return; /* first wins */
        h = (h + 1) & (g_by_va_cap - 1);
    }
    g_by_va[h] = idx + 1;
}

int static_boot_load(const char *dir, uint8_t *flat) {
    char path[1024];
    snprintf(path, sizeof path, "%s/boot.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    g_blob = (uint8_t *)malloc((size_t)n);
    if (!g_blob || fread(g_blob, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -2; }
    fclose(f);
    snprintf(path, sizeof path, "%s/boot.idx", dir);
    f = fopen(path, "r");
    if (!f) return -3;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'R') {
            unsigned a, s, p; long off; char kind[32];
            if (sscanf(line, "R %x %x %u %ld %31s", &a, &s, &p, &off, kind) != 5) { fclose(f); return -4; }
            if (off < 0 || off + (long)s > n || g_region_count >= 128) { fclose(f); return -5; }
            g_regions[g_region_count++] = (BootRegion){a, s, p, g_blob + off};
        } else if (line[0] == 'M') {
            unsigned i, h; char name[64];
            if (sscanf(line, "M %u %x %63s", &i, &h, name) != 3 || i != g_module_count || i >= 64) { fclose(f); return -6; }
            g_modules[g_module_count].handle = h;
            snprintf(g_modules[g_module_count].name, sizeof g_modules[0].name, "%s", name);
            g_modules[g_module_count].refs = 1;
            g_module_count++;
        } else if (line[0] == 'F') {            /* initial reference count of module i */
            unsigned i, r;
            if (sscanf(line, "F %u %u", &i, &r) != 2 || i >= g_module_count) { fclose(f); return -10; }
            g_modules[i].refs = r;
        } else if (line[0] == 'V') {            /* mapped PE view: base size entry */
            unsigned b, z, e;
            if (sscanf(line, "V %x %x %x", &b, &z, &e) != 3 || g_view_count >= 8) { fclose(f); return -11; }
            g_views[g_view_count][0] = b; g_views[g_view_count][1] = z; g_views[g_view_count][2] = e; g_view_count++;
        } else if (line[0] == 'E') {
            unsigned m, t, ord; char name[112];
            if (sscanf(line, "E %u %x %u %111s", &m, &t, &ord, name) != 4) { fclose(f); return -7; }
            if (g_export_count == g_export_cap) {
                g_export_cap = g_export_cap ? g_export_cap * 2 : 4096;
                g_exports = (BootExport *)realloc(g_exports, g_export_cap * sizeof *g_exports);
                if (!g_exports) { fclose(f); return -8; }
            }
            g_exports[g_export_count].module = m; g_exports[g_export_count].target = t;
            snprintf(g_exports[g_export_count].name, sizeof g_exports[0].name, "%s", name);
            g_export_count++;
        }
    }
    fclose(f);
    g_by_va_cap = 1; while (g_by_va_cap < g_export_count * 4 + 16) g_by_va_cap <<= 1;
    g_by_va = (uint32_t *)calloc(g_by_va_cap, sizeof *g_by_va);
    if (!g_by_va) return -9;
    for (uint32_t i = 0; i < g_export_count; i++) by_va_insert(i);
    (void)flat;
    g_loaded = 1;
    return 0;
}
int static_boot_loaded(void) { return g_loaded; }
uint32_t static_boot_region_count(void) { return g_region_count; }
int static_boot_region(uint32_t i, uint32_t *address, uint32_t *size, uint32_t *perms, const uint8_t **bytes) {
    if (i >= g_region_count) return -1;
    *address = g_regions[i].address; *size = g_regions[i].size; *perms = g_regions[i].perms; *bytes = g_regions[i].bytes;
    return 0;
}
uint32_t static_boot_module_count(void) { return g_module_count; }
const char *static_boot_module(uint32_t i, uint32_t *handle) {
    if (i >= g_module_count) return NULL;
    if (handle) *handle = g_modules[i].handle;
    return g_modules[i].name;
}
uint32_t static_boot_export_address(const char *dll, const char *name) {
    for (uint32_t i = 0; i < g_export_count; i++)
        if (!strcmp(g_exports[i].name, name) && g_exports[i].module < g_module_count &&
            !strcasecmp(g_modules[g_exports[i].module].name, dll))
            return g_exports[i].target;
    return 0;
}
int static_boot_export_at(uint32_t va, const char **dll, const char **name) {
    if (!g_loaded) return -1;
    uint32_t h = (va * 2654435761u) & (g_by_va_cap - 1);
    while (g_by_va[h]) {
        const BootExport *e = &g_exports[g_by_va[h] - 1];
        if (e->target == va) { *dll = g_modules[e->module].name; *name = e->name; return 0; }
        h = (h + 1) & (g_by_va_cap - 1);
    }
    return -1;
}
uint32_t static_boot_module_refs(uint32_t i) { return i < g_module_count ? g_modules[i].refs : 0; }
uint32_t static_boot_view_count(void) { return g_view_count; }
void static_boot_view(uint32_t i, uint32_t *base, uint32_t *size, uint32_t *entry) {
    *base = g_views[i][0]; *size = g_views[i][1]; *entry = g_views[i][2];
}
