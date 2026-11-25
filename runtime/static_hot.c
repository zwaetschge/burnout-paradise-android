/* Hot-discovery loader for the FAST engine (development host only).
 *
 * When the trampoline dispatches to a guest VA that static-full-v2 never lifted
 * as a function (an indirect `.trace` target), static_entry.c's recomp_lookup
 * miss path calls port_hot_resolve().  With PORT_HOT=1 in the environment we
 * ask a helper process (tools/static_hot_helper.py, given by PORT_HOT_HELPER)
 * to lift exactly that one function into a self-contained shared object, then
 * dlopen it, dlsym its sub_<VA>, register it, and hand it back so the run
 * continues.  Every VA is also recorded by the helper so one later static
 * re-lift can bake the whole set in ahead-of-time -- the shipped APK never runs
 * this loader.  Nothing is patched in the game image; a VA the helper cannot
 * lift returns 0 and the caller stops by name.
 *
 * This file is only active when PORT_HOT=1; otherwise port_hot_resolve() is a
 * cheap "return 0" and the engine behaves exactly as before.
 */
#include "recomp_types.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { uint32_t va; recomp_func_t fn; } hot_entry;

static hot_entry *g_hot;            /* sorted by va */
static uint32_t   g_hot_n, g_hot_cap;
static FILE      *g_to_helper;      /* we write VAs here */
static FILE      *g_from_helper;    /* we read replies here */
static int        g_enabled = -1;   /* -1 unknown, 0 off, 1 on */
static uint64_t   g_found, g_failed, g_requests;

static recomp_func_t hot_find(uint32_t va) {
    uint32_t lo = 0, hi = g_hot_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_hot[mid].va < va) lo = mid + 1; else hi = mid;
    }
    if (lo < g_hot_n && g_hot[lo].va == va) return g_hot[lo].fn;
    return NULL;
}

static void hot_insert(uint32_t va, recomp_func_t fn) {
    if (g_hot_n == g_hot_cap) {
        g_hot_cap = g_hot_cap ? g_hot_cap * 2 : 1024;
        g_hot = (hot_entry *)realloc(g_hot, g_hot_cap * sizeof *g_hot);
        if (!g_hot) { perror("hot realloc"); _exit(70); }
    }
    uint32_t lo = 0, hi = g_hot_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_hot[mid].va < va) lo = mid + 1; else hi = mid;
    }
    memmove(&g_hot[lo + 1], &g_hot[lo], (g_hot_n - lo) * sizeof *g_hot);
    g_hot[lo].va = va; g_hot[lo].fn = fn; g_hot_n++;
}

/* Spawn the helper with two pipes; wait for its READY line. */
static int hot_start(const char *cmd) {
    int to[2], from[2];
    if (pipe(to) || pipe(from)) { perror("pipe"); return 0; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 0; }
    if (pid == 0) {
        dup2(to[0], 0); dup2(from[1], 1);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(to[0]); close(from[1]);
    g_to_helper = fdopen(to[1], "w");
    g_from_helper = fdopen(from[0], "r");
    if (!g_to_helper || !g_from_helper) return 0;
    char line[256];
    if (!fgets(line, sizeof line, g_from_helper)) { fprintf(stderr, "PORT_HOT helper gave no READY\n"); return 0; }
    if (strncmp(line, "READY", 5) != 0) { fprintf(stderr, "PORT_HOT helper: %s", line); return 0; }
    fprintf(stderr, "PORT_HOT helper ready: %s\n", cmd);
    return 1;
}

static void hot_setup(void) {
    const char *on = getenv("PORT_HOT");
    if (!on || strcmp(on, "1") != 0) { g_enabled = 0; return; }
    const char *cmd = getenv("PORT_HOT_HELPER");
    if (!cmd || !*cmd) { fprintf(stderr, "PORT_HOT=1 but PORT_HOT_HELPER unset\n"); g_enabled = 0; return; }
    g_enabled = hot_start(cmd) ? 1 : 0;
}

/* Load one shard described as "<so> <count> <va> <va> ..." (or the single
 * form "<so> sub_<VA> 1") and register its functions. Returns `want`'s. */
static recomp_func_t hot_register(char *desc, uint32_t want) {
    char so[768]; int used = 0;
    if (sscanf(desc, "%767s %n", so, &used) != 1) return NULL;
    char *rest = desc + used;
    void *h = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "PORT_HOT dlopen %s: %s\n", so, dlerror()); return NULL; }
    recomp_func_t wanted = NULL;
    if (strncmp(rest, "sub_", 4) == 0) {
        char name[64];
        if (sscanf(rest, "%63s", name) != 1) return NULL;
        wanted = (recomp_func_t)dlsym(h, name);
        if (wanted && !hot_find(want)) { hot_insert(want, wanted); g_found++; }
        return wanted;
    }
    char *q = rest;
    unsigned long count = strtoul(q, &q, 10);
    for (unsigned long i = 0; i < count; i++) {
        uint32_t a = (uint32_t)strtoul(q, &q, 16);
        char name[32]; snprintf(name, sizeof name, "sub_%08X", a);
        recomp_func_t f = (recomp_func_t)dlsym(h, name);
        if (!f) { fprintf(stderr, "PORT_HOT dlsym %s: %s\n", name, dlerror()); continue; }
        if (a == want) wanted = f;
        if (!hot_find(a)) { hot_insert(a, f); g_found++; }
    }
    return wanted;
}

/* PORT_HOT_PRELOAD=<pack.idx>: shards built ahead of time (tools/static_pfunc_pack.py). */
static void hot_preload(void) {
    const char *idx = getenv("PORT_HOT_PRELOAD");
    if (!idx || !*idx) return;
    FILE *f = fopen(idx, "r");
    if (!f) { fprintf(stderr, "PORT_HOT preload: cannot open %s\n", idx); return; }
    static char line[1 << 16];
    unsigned shards = 0;
    while (fgets(line, sizeof line, f)) { if (line[0] > ' ') { hot_register(line, 0); shards++; } }
    fclose(f);
    fprintf(stderr, "PORT_HOT preload: %u shards, %llu functions from %s\n", shards, (unsigned long long)g_found, idx);
}

/* ---- code the game loads into its heap at run time ---------------------- */
/* The original image keeps part of its sound logic as relocated x86 code inside
 * data bundles (AEMS banks) and calls it through function pointers. Such a
 * target is not in the image: the function is lifted from the live guest
 * memory, and every later use first checks that the bytes it was lifted from
 * are still there (a bank can be unloaded and another loaded at the address). */
uint8_t *port_flat_base(void);
int port_va_in_image(uint32_t va);
typedef struct { uint32_t va, lo, hi; recomp_func_t fn; uint8_t *bytes; } dyn_entry;
static dyn_entry *g_dyn;
static uint32_t g_dyn_n, g_dyn_cap;
static uint64_t g_dyn_lifted;

/* Functions lifted ahead of time from an earlier discovery run
 * (tools/static_dyn_pack.py); absent unless the image was built with them. */
typedef struct { uint32_t va, lo, hi; const unsigned char *bytes; void (*fn)(void);
                 unsigned movable; const unsigned short *ext, *inner; } PortDynEntry;
/* Load address of the dynamic function being entered minus the address it was
 * lifted at (tools/static_dyn_pack.py makes the movable ones base-relative). */
RECOMP_TLS uint32_t port_dyn_delta;
/* Does the guest code at lo+d equal the lifted bytes, once the fields that
 * differ by load address are corrected? ext fields (rel32 out of the function)
 * are smaller by d, inner fields (absolute addresses into it) larger by d. */
static int dyn_moved_match(const PortDynEntry *e, const uint8_t *flat, uint32_t d) {
    uint32_t len = e->hi - e->lo;
    uint8_t stack[4096], *tmp = len <= sizeof stack ? stack : (uint8_t *)malloc(len);
    if (!tmp) return 0;
    memcpy(tmp, flat + (uint32_t)(e->lo + d), len);
    for (const unsigned short *o = e->ext; *o != 0xFFFF; o++) {
        if (*o + 4u > len) { if (tmp != stack) free(tmp); return 0; }
        uint32_t v; memcpy(&v, tmp + *o, 4); v += d; memcpy(tmp + *o, &v, 4);
    }
    for (const unsigned short *o = e->inner; *o != 0xFFFF; o++) {
        if (*o + 4u > len) { if (tmp != stack) free(tmp); return 0; }
        uint32_t v; memcpy(&v, tmp + *o, 4); v -= d; memcpy(tmp + *o, &v, 4);
    }
    int same = !memcmp(tmp, e->bytes, len);
    if (tmp != stack) free(tmp);
    return same;
}
extern const PortDynEntry port_dyn_table[] __attribute__((weak));
extern const unsigned port_dyn_count __attribute__((weak));

static recomp_func_t dyn_resolve(uint32_t va) {
    const uint8_t *flat = port_flat_base();
    for (uint32_t i = 0; i < g_dyn_n; i++)
        if (g_dyn[i].va == va && !memcmp(flat + g_dyn[i].lo, g_dyn[i].bytes, g_dyn[i].hi - g_dyn[i].lo)) return g_dyn[i].fn;
    if (&port_dyn_count && port_dyn_table) {
        int same_address = 0;
        for (unsigned i = 0; i < port_dyn_count; i++) {
            const PortDynEntry *e = &port_dyn_table[i];
            if (e->va != va) continue;
            same_address = 1;
            if (!memcmp(flat + e->lo, e->bytes, e->hi - e->lo)) { port_dyn_delta = 0; return (recomp_func_t)e->fn; }
        }
        /* The same code loaded at another address (other banks loaded first). */
        for (unsigned i = 0; i < port_dyn_count; i++) {
            const PortDynEntry *e = &port_dyn_table[i];
            if (!e->movable || e->va == va) continue;
            uint32_t d = va - e->va;
            if (dyn_moved_match(e, flat, d)) {
                static int logged;
                if (logged++ < 16) fprintf(stderr, "PORT_DYN 0x%08X: translated code of 0x%08X, moved by %+d\n", va, e->va, (int)d);
                port_dyn_delta = d;
                return (recomp_func_t)e->fn;
            }
        }
        if (same_address) fprintf(stderr, "PORT_DYN 0x%08X: guest code differs from every translated version\n", va);
    }
    if (g_enabled < 0) { hot_preload(); hot_setup(); }
    uint32_t base = (va & ~0xFFFu) > 0x18000u ? (va & ~0xFFFu) - 0x8000u : 0x10000u;
    if (!g_enabled) {
        /* No translator here (the device): keep the same 0x28000-byte window
         * the host helper would get, so the function can be lifted later
         * (tools/static_hot_helper.py "DYN <va> <base> <file>", then
         * tools/static_dyn_pack.py). Private file: the code comes from the
         * user's sound banks. Directory: PORT_DYN_CAPTURE, else next to the
         * shader cache. */
        char dir[900] = "";
        const char *capture = getenv("PORT_DYN_CAPTURE"), *cache = getenv("PORT_SHADER_CACHE");
        if (capture && *capture) snprintf(dir, sizeof dir, "%s", capture);
        else if (cache && *cache) {
            snprintf(dir, sizeof dir, "%s", cache);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
        }
        if (dir[0]) {
            char file[1024];
            snprintf(file, sizeof file, "%s/dyn-window-%08X-%08X.bin", dir, va, base);
            FILE *w = fopen(file, "wb");
            if (w) {
                fwrite(flat + base, 1, 0x28000u, w);
                fclose(w);
                fprintf(stderr, "PORT_DYN capture: code at 0x%08X is not translated; window 0x%08X+0x28000 saved to %s\n", va, base, file);
            }
        }
        return NULL;
    }
    const char *out = getenv("PORT_HOT_OUT");
    char path[1024];
    snprintf(path, sizeof path, "%s/dyn-window.bin", out ? out : "/tmp");
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    fwrite(flat + base, 1, 0x28000u, f);
    fclose(f);
    fprintf(g_to_helper, "DYN 0x%08X 0x%08X %s\n", va, base, path);
    fflush(g_to_helper);
    char line[8192];
    if (!fgets(line, sizeof line, g_from_helper)) { fprintf(stderr, "PORT_HOT helper died\n"); g_enabled = 0; return NULL; }
    char so[768], name[64]; unsigned count = 0, lo = 0, hi = 0;
    if (sscanf(line, "OK %767s %63s %u %x %x", so, name, &count, &lo, &hi) != 5 || hi <= lo || va < lo || va >= hi) {
        fprintf(stderr, "PORT_HOT dynamic code 0x%08X: %s", va, line);
        return NULL;
    }
    void *h = dlopen(so, RTLD_NOW | RTLD_GLOBAL);
    recomp_func_t fn = h ? (recomp_func_t)dlsym(h, name) : NULL;
    if (!fn) { fprintf(stderr, "PORT_HOT dynamic code 0x%08X: cannot load %s (%s)\n", va, so, dlerror()); return NULL; }
    if (g_dyn_n == g_dyn_cap) {
        g_dyn_cap = g_dyn_cap ? g_dyn_cap * 2 : 256;
        g_dyn = (dyn_entry *)realloc(g_dyn, g_dyn_cap * sizeof *g_dyn);
        if (!g_dyn) { perror("dyn realloc"); _exit(70); }
    }
    dyn_entry *e = &g_dyn[g_dyn_n++];
    e->va = va; e->lo = lo; e->hi = hi; e->fn = fn;
    e->bytes = (uint8_t *)malloc(hi - lo);
    memcpy(e->bytes, flat + lo, hi - lo);
    if (++g_dyn_lifted % 25 == 1) fprintf(stderr, "PORT_HOT dynamic code: %llu functions lifted from guest memory (last 0x%08X, %u bytes)\n", (unsigned long long)g_dyn_lifted, va, hi - lo);
    return fn;
}

/* Called from recomp_lookup's miss path. Returns a function for `va` or 0. */
#define HOT_FAILED ((recomp_func_t)(uintptr_t)1)
recomp_func_t port_hot_resolve(uint32_t va) {
    if (!port_va_in_image(va) && va >= 0x02000000u && va < 0x60000000u &&   /* guest heap / VirtualAlloc arenas ... */
        (va < 0x10001000u || va >= 0x100C0000u)) return dyn_resolve(va);     /* ... but not dbdata.dll */
    if ((va < 0x00401000u || va >= 0x08400000u) &&            /* game image ... */
        (va < 0x10001000u || va >= 0x100C0000u)) return NULL;   /* ... or dbdata.dll */
    if (g_enabled < 0) { hot_preload(); hot_setup(); }
    recomp_func_t fn = hot_find(va);
    if (fn) return fn == HOT_FAILED ? NULL : fn;
    if (!g_enabled) return NULL;

    fprintf(g_to_helper, "0x%08X\n", va);
    fflush(g_to_helper);
    char line[8192];
    if (!fgets(line, sizeof line, g_from_helper)) { fprintf(stderr, "PORT_HOT helper died\n"); g_enabled = 0; return NULL; }

    if (strncmp(line, "OK ", 3) == 0) {
        recomp_func_t wanted = hot_register(line + 3, va);
        if (!wanted) { fprintf(stderr, "PORT_HOT reply unusable for 0x%08X: %.200s", va, line); g_failed++; hot_insert(va, HOT_FAILED); return NULL; }
        g_requests++;
        if (g_requests % 100 == 0)
            fprintf(stderr, "PORT_HOT progress: %llu requests, %llu functions loaded (last 0x%08X)\n",
                    (unsigned long long)g_requests, (unsigned long long)g_found, va);
        return wanted;
    }
    /* FAIL <reason> */
    fprintf(stderr, "PORT_HOT lift 0x%08X: %s", va, line + (strncmp(line, "FAIL ", 5) == 0 ? 5 : 0));
    g_failed++;
    hot_insert(va, HOT_FAILED);
    return NULL;
}
