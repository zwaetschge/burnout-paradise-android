/* A19 -- the kernel32 semantics the static image was still missing.
 * See runtime/guest_kernel_misc.h for the full contract and the list of
 * documented deviations. Nothing here fabricates a Win32 success: an operation
 * this runtime cannot model returns a GM_* / named stop.
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. Kernel-object
 * handles come from runtime/guest_handles.h; no host pointer is ever written
 * into guest memory. */
#define _GNU_SOURCE
#include "guest_kernel_misc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "guest_handles.h"
#include "guest_shared_data.h"

/* The file API owns this struct (runtime/guest_file_api.c); only its first
 * member -- the host descriptor -- is read here, and only after
 * gh_lookup() confirmed the handle is a GH_KIND_FILE entry. Everything else
 * goes through the file API's own entry points. */
typedef struct { int fd; } GkmFilePrefix;

#define GKM_MAX_STRING 32768u
#define GKM_MAX_WSTRING 16384u
#define GKM_MAX_ENV 32u
#define GKM_MAX_NAME 256u
#define GKM_MAX_PATH 1024u

/* HEAP_ZERO_MEMORY for the process heap (runtime/guest_process_heap.h). */
#define GKM_HEAP_ZERO 0x8u

typedef struct {
    uint32_t block;      /* guest address of the payload */
    uint32_t size;       /* requested size in bytes */
    uint32_t lockcount;
    uint32_t flags;      /* the flags the caller passed */
} GkmGlobal;

typedef struct {
    char *name;
    char *value;         /* empty string for "delete this variable" */
    int deleted;
} GkmEnv;

struct GuestKernelMisc {
    GuestMemory *mem;
    uint32_t teb;
    GuestProcessHeap *heap;
    GuestPathMap *map;
    const GuestLocale *locale;
    const GuestStringCompare *strcmp;
    const GuestEnvironmentEntry *env;
    uint32_t env_count;

    uint32_t processor_count;
    uint32_t affinity_mask;

    uint32_t exit_code;
    uint32_t unhandled_filter;
    uint32_t raise_count;
    uint32_t debug_strings;
    uint32_t global_blocks;
    uint32_t moveable_handles;
    uint32_t env_overrides;
    uint32_t edx_valid;
    uint32_t edx;

    int32_t thread_priority;
    uint32_t thread_affinity;
    uint32_t ideal_processor;

    char computer[64];
    char user[64];
    char last_log[512];

    GkmEnv env_overlay[GKM_MAX_ENV];
};

/* ---------------------------------------------------------------- helpers */

static int put_u32(GuestMemory *m, uint32_t at, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
    return gm_write(m, at, b, 4);
}

static int get_u32(GuestMemory *m, uint32_t at, uint32_t *v) {
    uint8_t b[4];
    int rc = gm_read(m, at, b, 4);
    if (rc) return rc;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
    return 0;
}

static void set_error(GuestKernelMisc *km, uint32_t error) {
    if (!km->mem || !km->teb) return;
    put_u32(km->mem, km->teb + 0x34, error);
}

static void log_line(GuestKernelMisc *km, const char *text) {
    snprintf(km->last_log, sizeof km->last_log, "%s", text ? text : "");
}

/* Read a NUL-terminated CP1252 string into a host buffer. Stops (GM_*) on a
 * guest fault, a missing terminator inside the bound, or a byte the installed
 * code page cannot decode. */
static int read_a(GuestKernelMisc *km, uint32_t address, char *out, size_t cap,
                  size_t *len) {
    if (!address) return GM_RANGE;
    size_t n = 0;
    for (; n + 1 < cap; n++) {
        uint8_t c = 0;
        int rc = gm_read(km->mem, address + (uint32_t)n, &c, 1);
        if (rc) return rc;
        if (!c) {
            out[n] = 0;
            if (len) *len = n;
            return 0;
        }
        if (km->locale && c >= 0x80) {
            uint32_t uni = 0;
            if (gl_decode_byte(km->locale, c, &uni) || !uni)
                return GM_UNSUPPORTED;
        }
        out[n] = (char)c;
    }
    return GM_UNSUPPORTED;
}

/* Read a NUL-terminated UTF-16 string into a host buffer. */
static int read_w(GuestKernelMisc *km, uint32_t address, uint16_t *out,
                  size_t cap, size_t *len) {
    if (!address) return GM_RANGE;
    size_t n = 0;
    for (; n + 1 < cap; n++) {
        uint8_t b[2];
        int rc = gm_read(km->mem, address + (uint32_t)n * 2, b, 2);
        if (rc) return rc;
        uint16_t c = (uint16_t)(b[0] | (b[1] << 8));
        if (!c) {
            out[n] = 0;
            if (len) *len = n;
            return 0;
        }
        out[n] = c;
    }
    return GM_UNSUPPORTED;
}

/* Read a UTF-16 guest string and narrow it to ASCII (paths only: the path
 * projection is ASCII/CP1252). */
static int read_w_ascii(GuestKernelMisc *km, uint32_t address, char *out,
                        size_t cap, size_t *len) {
    uint16_t wide[GKM_MAX_WSTRING];
    size_t wlen = 0;
    int rc = read_w(km, address, wide, sizeof wide / sizeof wide[0], &wlen);
    if (rc) return rc;
    if (wlen + 1 > cap) return GM_UNSUPPORTED;
    for (size_t i = 0; i <= wlen; i++) {
        if (wide[i] > 0x7f) return GM_UNSUPPORTED;
        out[i] = (char)wide[i];
    }
    if (len) *len = wlen;
    return 0;
}

static int write_a(GuestKernelMisc *km, uint32_t address, const char *text,
                   size_t count) {
    return gm_write(km->mem, address, text, (uint32_t)count);
}

static int write_w(GuestKernelMisc *km, uint32_t address, const uint16_t *text,
                   size_t count) {
    uint8_t *bytes = (uint8_t *)malloc(count * 2 ? count * 2 : 1);
    if (!bytes) return GM_ALLOCATION;
    for (size_t i = 0; i < count; i++) {
        bytes[i * 2] = (uint8_t)text[i];
        bytes[i * 2 + 1] = (uint8_t)(text[i] >> 8);
    }
    int rc = gm_write(km->mem, address, bytes, (uint32_t)(count * 2));
    free(bytes);
    return rc;
}

static int ascii_to_wide(const char *in, uint16_t *out, size_t cap,
                         size_t *len) {
    size_t n = 0;
    for (; in[n]; n++) {
        if (n + 1 >= cap) return GM_UNSUPPORTED;
        if ((uint8_t)in[n] >= 0x80) return GM_UNSUPPORTED;
        out[n] = (uint8_t)in[n];
    }
    out[n] = 0;
    if (len) *len = n;
    return 0;
}

/* ------------------------------------------------------- global/local heap */

static GkmGlobal *gkm_global_record(uint32_t handle) {
    return (GkmGlobal *)gh_lookup(handle, GH_KIND_OTHER, NULL);
}

static void gkm_global_destroy(void *object) {
    GkmGlobal *g = (GkmGlobal *)object;
    free(g);
}

/* ALLOC: flags/bytes -> handle or pointer. */
static int gkm_alloc_common(GuestKernelMisc *km, uint32_t flags, uint32_t bytes,
                            uint32_t *result) {
    if (!km->heap) return GM_UNSUPPORTED;
    if (flags & GKM_GMEM_UNSUPPORTED) return GM_UNSUPPORTED;
    /* NOCOMPACT/NODISCARD/LOWER/SHARE are documented as ignorable on the
     * modern heap, so they are accepted and not recorded. */
    if (flags & ~(GKM_GMEM_MOVEABLE | GKM_GMEM_ZEROINIT | GKM_GMEM_SHARE |
                  GKM_GMEM_IGNORED))
        return GM_UNSUPPORTED;
    uint32_t block = 0;
    int rc = gph_alloc(km->heap, GPH_HANDLE,
                       (flags & GKM_GMEM_ZEROINIT) ? GKM_HEAP_ZERO : 0u, bytes,
                       &block);
    if (rc) return rc;
    if (!block) {
        set_error(km, GKM_ERROR_NOT_ENOUGH_MEMORY);
        *result = 0;
        return GM_OK;
    }
    km->global_blocks++;
    if (!(flags & GKM_GMEM_MOVEABLE)) {
        *result = block;
        return GM_OK;
    }
    GkmGlobal *g = (GkmGlobal *)calloc(1, sizeof *g);
    if (!g) {
        gph_free(km->heap, GPH_HANDLE, 0, block);
        km->global_blocks--;
        return GM_ALLOCATION;
    }
    g->block = block;
    g->size = bytes;
    g->lockcount = 0;
    g->flags = flags;
    uint32_t handle = gh_insert(GH_KIND_OTHER, g, gkm_global_destroy);
    if (!handle) {
        gph_free(km->heap, GPH_HANDLE, 0, block);
        km->global_blocks--;
        free(g);
        set_error(km, GKM_ERROR_NOT_ENOUGH_MEMORY);
        *result = 0;
        return GM_OK;
    }
    km->moveable_handles++;
    *result = handle;
    return GM_OK;
}

/* A guest handle is a small even value from the shared table; a GMEM_FIXED
 * block is a guest address inside the flat reservation. Windows makes the same
 * distinction (handle vs. pointer). */
static int gkm_is_handle(uint32_t value) { return value >= GH_FIRST_HANDLE &&
                                                  value < GH_FIRST_HANDLE + GH_MAX_HANDLES; }

static int gkm_free_common(GuestKernelMisc *km, uint32_t value,
                           uint32_t *result) {
    if (!value) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    if (gkm_is_handle(value)) {
        GkmGlobal *g = gkm_global_record(value);
        if (!g) {
            set_error(km, GKM_ERROR_INVALID_HANDLE);
            *result = 0;
            return GM_OK;
        }
        int rc = gph_free(km->heap, GPH_HANDLE, 0, g->block);
        if (rc) return rc;
        gh_close(value);              /* runs gkm_global_destroy */
        km->global_blocks--;
        km->moveable_handles--;
        *result = 0;
        return GM_OK;
    }
    int rc = gph_free(km->heap, GPH_HANDLE, 0, value);
    if (rc) return rc;                /* foreign pointer: named stop */
    km->global_blocks--;
    *result = 0;
    return GM_OK;
}

int gkm_global_alloc(GuestKernelMisc *km, const uint32_t *args,
                     uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    return gkm_alloc_common(km, args[0], args[1], result);
}

int gkm_global_free(GuestKernelMisc *km, const uint32_t *args,
                    uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    return gkm_free_common(km, args[0], result);
}

int gkm_global_lock(GuestKernelMisc *km, const uint32_t *args,
                    uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t value = args[0];
    if (!value) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    if (gkm_is_handle(value)) {
        GkmGlobal *g = gkm_global_record(value);
        if (!g) {
            set_error(km, GKM_ERROR_INVALID_HANDLE);
            *result = 0;
            return GM_OK;
        }
        g->lockcount++;
        *result = g->block;
        return GM_OK;
    }
    /* A fixed block cannot be locked or unlocked: documented FALSE with
     * LastError == NO_ERROR. */
    set_error(km, GKM_ERROR_SUCCESS);
    *result = 0;
    return GM_OK;
}

int gkm_global_unlock(GuestKernelMisc *km, const uint32_t *args,
                      uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t value = args[0];
    if (!value) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    if (gkm_is_handle(value)) {
        GkmGlobal *g = gkm_global_record(value);
        if (!g) {
            set_error(km, GKM_ERROR_INVALID_HANDLE);
            *result = 0;
            return GM_OK;
        }
        if (!g->lockcount) {
            set_error(km, GKM_ERROR_SUCCESS);
            *result = 0;
            return GM_OK;
        }
        g->lockcount--;
        /* Documented quirk: TRUE while locks remain, FALSE with
         * LastError == NO_ERROR when the last lock is released. */
        set_error(km, GKM_ERROR_SUCCESS);
        *result = g->lockcount ? 1u : 0u;
        return GM_OK;
    }
    set_error(km, GKM_ERROR_SUCCESS);
    *result = 0;
    return GM_OK;
}

int gkm_global_size(GuestKernelMisc *km, const uint32_t *args,
                    uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t value = args[0];
    if (!value) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0xffffffffu;
        return GM_OK;
    }
    if (gkm_is_handle(value)) {
        GkmGlobal *g = gkm_global_record(value);
        if (!g) {
            set_error(km, GKM_ERROR_INVALID_HANDLE);
            *result = 0xffffffffu;
            return GM_OK;
        }
        *result = g->size;
        return GM_OK;
    }
    uint32_t size = 0;
    int rc = gph_size(km->heap, GPH_HANDLE, 0, value, &size);
    if (rc) return rc;
    *result = size;
    return GM_OK;
}

int gkm_global_flags(GuestKernelMisc *km, const uint32_t *args,
                     uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t value = args[0];
    if (!value) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0xffffffffu;
        return GM_OK;
    }
    if (gkm_is_handle(value)) {
        GkmGlobal *g = gkm_global_record(value);
        if (!g) {
            set_error(km, GKM_ERROR_INVALID_HANDLE);
            *result = 0xffffffffu;
            return GM_OK;
        }
        /* Documented: the low-order byte of the GlobalFlags result is the lock
         * count; the block type lives in the bits above it. */
        *result = (g->flags & ~GKM_GMEM_LOCKCOUNT) |
                  (g->lockcount & GKM_GMEM_LOCKCOUNT);
        return GM_OK;
    }
    uint32_t size = 0;
    int rc = gph_size(km->heap, GPH_HANDLE, 0, value, &size);
    if (rc) return rc;
    (void)size;
    *result = GKM_GMEM_FIXED;
    return GM_OK;
}

int gkm_global_realloc(GuestKernelMisc *km, const uint32_t *args,
                       uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t value = args[0], bytes = args[1], flags = args[2];
    if (!value) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    if (!gkm_is_handle(value)) {
        /* A fixed block is reallocated in place: the heap may move it, and the
         * caller's pointer changes, which is exactly the documented
         * GMEM_FIXED/GlobalReAlloc contract. */
        uint32_t moved = 0;
        int rc = gph_realloc(km->heap, GPH_HANDLE,
                             (flags & GKM_GMEM_ZEROINIT) ? GKM_HEAP_ZERO : 0u,
                             value, bytes, &moved);
        if (rc) return rc;
        if (!moved) {
            set_error(km, GKM_ERROR_NOT_ENOUGH_MEMORY);
            *result = 0;
            return GM_OK;
        }
        *result = moved;
        return GM_OK;
    }
    GkmGlobal *g = gkm_global_record(value);
    if (!g) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    uint32_t moved = 0;
    int rc = gph_realloc(km->heap, GPH_HANDLE,
                         (flags & GKM_GMEM_ZEROINIT) ? GKM_HEAP_ZERO : 0u,
                         g->block, bytes, &moved);
    if (rc) return rc;
    if (!moved) {
        set_error(km, GKM_ERROR_NOT_ENOUGH_MEMORY);
        *result = 0;
        return GM_OK;
    }
    g->block = moved;
    g->size = bytes;
    *result = value;
    return GM_OK;
}

/* -------------------------------------------------------------- lstr* A/W */

int gkm_lstrlen_a(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    char text[GKM_MAX_STRING];
    size_t len = 0;
    int rc = read_a(km, args[0], text, sizeof text, &len);
    if (rc) return rc;
    *result = (uint32_t)len;
    return GM_OK;
}

int gkm_lstrlen_w(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint16_t text[GKM_MAX_WSTRING];
    size_t len = 0;
    int rc = read_w(km, args[0], text, sizeof text / sizeof text[0], &len);
    if (rc) return rc;
    *result = (uint32_t)len;
    return GM_OK;
}

int gkm_lstrcpy_a(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    char text[GKM_MAX_STRING];
    size_t len = 0;
    int rc = read_a(km, args[1], text, sizeof text, &len);
    if (rc) return rc;
    rc = write_a(km, args[0], text, len + 1);
    if (rc) return rc;
    *result = args[0];
    return GM_OK;
}

int gkm_lstrcpy_w(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint16_t text[GKM_MAX_WSTRING];
    size_t len = 0;
    int rc = read_w(km, args[1], text, sizeof text / sizeof text[0], &len);
    if (rc) return rc;
    rc = write_w(km, args[0], text, len + 1);
    if (rc) return rc;
    *result = args[0];
    return GM_OK;
}

int gkm_lstrcpyn_a(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t dest = args[0], source = args[1];
    int32_t count = (int32_t)args[2];
    if (count <= 0) {
        *result = dest;
        return GM_OK;
    }
    char text[GKM_MAX_STRING];
    size_t len = 0;
    int rc = read_a(km, source, text, sizeof text, &len);
    if (rc) return rc;
    size_t copy = (size_t)count - 1;
    if (copy > len) copy = len;
    text[copy] = 0;
    rc = write_a(km, dest, text, copy + 1);
    if (rc) return rc;
    *result = dest;
    return GM_OK;
}

int gkm_lstrcpyn_w(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t dest = args[0], source = args[1];
    int32_t count = (int32_t)args[2];
    if (count <= 0) {
        *result = dest;
        return GM_OK;
    }
    uint16_t text[GKM_MAX_WSTRING];
    size_t len = 0;
    int rc = read_w(km, source, text, sizeof text / sizeof text[0], &len);
    if (rc) return rc;
    size_t copy = (size_t)count - 1;
    if (copy > len) copy = len;
    text[copy] = 0;
    rc = write_w(km, dest, text, copy + 1);
    if (rc) return rc;
    *result = dest;
    return GM_OK;
}

int gkm_lstrcat_a(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t dest = args[0];
    char left[GKM_MAX_STRING], right[GKM_MAX_STRING];
    size_t llen = 0, rlen = 0;
    int rc = read_a(km, dest, left, sizeof left, &llen);
    if (rc) return rc;
    rc = read_a(km, args[1], right, sizeof right, &rlen);
    if (rc) return rc;
    if (llen + rlen + 1 > sizeof left) return GM_UNSUPPORTED;
    memcpy(left + llen, right, rlen + 1);
    rc = write_a(km, dest, left, llen + rlen + 1);
    if (rc) return rc;
    *result = dest;
    return GM_OK;
}

int gkm_lstrcat_w(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t dest = args[0];
    uint16_t left[GKM_MAX_WSTRING], right[GKM_MAX_WSTRING];
    size_t llen = 0, rlen = 0;
    int rc = read_w(km, dest, left, sizeof left / sizeof left[0], &llen);
    if (rc) return rc;
    rc = read_w(km, args[1], right, sizeof right / sizeof right[0], &rlen);
    if (rc) return rc;
    if (llen + rlen + 1 > sizeof left / sizeof left[0]) return GM_UNSUPPORTED;
    memcpy(left + llen, right, (rlen + 1) * sizeof left[0]);
    rc = write_w(km, dest, left, llen + rlen + 1);
    if (rc) return rc;
    *result = dest;
    return GM_OK;
}

/* lstrcmpA compares the CP1252 bytes; for ASCII this is the documented
 * character order. The result is the difference of the first differing byte
 * (or of the lengths), which is what a Win32 caller uses as <0/0/>0. */
int gkm_lstrcmp_a(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    char left[GKM_MAX_STRING], right[GKM_MAX_STRING];
    size_t llen = 0, rlen = 0;
    int rc = read_a(km, args[0], left, sizeof left, &llen);
    if (rc) return rc;
    rc = read_a(km, args[1], right, sizeof right, &rlen);
    if (rc) return rc;
    size_t n = llen < rlen ? llen : rlen;
    for (size_t i = 0; i < n; i++) {
        unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
        if (a != b) {
            *result = (uint32_t)(int32_t)((int)a - (int)b);
            return GM_OK;
        }
    }
    *result = (uint32_t)(int32_t)((int)llen - (int)rlen);
    return GM_OK;
}

/* lstrcmpW compares UTF-16 code units (no collation). */
int gkm_lstrcmp_w(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint16_t left[GKM_MAX_WSTRING], right[GKM_MAX_WSTRING];
    size_t llen = 0, rlen = 0;
    int rc = read_w(km, args[0], left, sizeof left / sizeof left[0], &llen);
    if (rc) return rc;
    rc = read_w(km, args[1], right, sizeof right / sizeof right[0], &rlen);
    if (rc) return rc;
    size_t n = llen < rlen ? llen : rlen;
    for (size_t i = 0; i < n; i++) {
        if (left[i] != right[i]) {
            *result = (uint32_t)(int32_t)((int)left[i] - (int)right[i]);
            return GM_OK;
        }
    }
    *result = (uint32_t)(int32_t)((int)llen - (int)rlen);
    return GM_OK;
}

/* lstrcmpiA: both operands are converted to UTF-16 in guest scratch memory and
 * compared with the captured sort-NLS casing map -- the same table lstrcmpiW
 * uses. Without that table this stops instead of consulting a host locale. */
int gkm_lstrcmpi_a(GuestKernelMisc *km, const uint32_t *args,
                   uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!km->strcmp || !km->heap) return GM_UNSUPPORTED;
    char left[GKM_MAX_STRING], right[GKM_MAX_STRING];
    size_t llen = 0, rlen = 0;
    int rc = read_a(km, args[0], left, sizeof left, &llen);
    if (rc) return rc;
    rc = read_a(km, args[1], right, sizeof right, &rlen);
    if (rc) return rc;
    uint16_t wide[GKM_MAX_WSTRING];
    size_t wlen = 0;
    rc = ascii_to_wide(left, wide, sizeof wide / sizeof wide[0], &wlen);
    if (rc) return rc;
    uint32_t left_va = 0, right_va = 0;
    rc = gph_alloc(km->heap, GPH_HANDLE, GKM_HEAP_ZERO,
                   (uint32_t)((wlen + 1) * 2), &left_va);
    if (rc) return rc;
    if (!left_va) return GM_ALLOCATION;
    rc = write_w(km, left_va, wide, wlen + 1);
    if (rc) {
        gph_free(km->heap, GPH_HANDLE, 0, left_va);
        return rc;
    }
    rc = ascii_to_wide(right, wide, sizeof wide / sizeof wide[0], &wlen);
    if (rc) {
        gph_free(km->heap, GPH_HANDLE, 0, left_va);
        return rc;
    }
    rc = gph_alloc(km->heap, GPH_HANDLE, GKM_HEAP_ZERO,
                   (uint32_t)((wlen + 1) * 2), &right_va);
    if (rc || !right_va) {
        gph_free(km->heap, GPH_HANDLE, 0, left_va);
        return rc ? rc : GM_ALLOCATION;
    }
    rc = write_w(km, right_va, wide, wlen + 1);
    if (rc) {
        gph_free(km->heap, GPH_HANDLE, 0, left_va);
        gph_free(km->heap, GPH_HANDLE, 0, right_va);
        return rc;
    }
    int32_t value = 0;
    rc = gls_compare(km->strcmp, km->mem, left_va, right_va, &value);
    gph_free(km->heap, GPH_HANDLE, 0, left_va);
    gph_free(km->heap, GPH_HANDLE, 0, right_va);
    if (rc) return rc;
    *result = (uint32_t)value;
    return GM_OK;
}

/* ------------------------------------------------------------ system info */

int gkm_get_system_info(GuestKernelMisc *km, const uint32_t *args,
                        uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t out = args[0];
    if (!out) return GM_RANGE;
    int rc = gm_check(km->mem, out, GKM_SYSTEM_INFO_SIZE, GM_WRITE);
    if (rc) return rc;
    uint8_t b[GKM_SYSTEM_INFO_SIZE];
    memset(b, 0, sizeof b);
    /* wProcessorArchitecture = PROCESSOR_ARCHITECTURE_INTEL, wReserved = 0 */
    b[0] = 0; b[1] = 0;
    uint32_t page = 4096, min_app = 0x00010000u, max_app = 0x7FFEFFFFu;
    uint32_t granularity = 65536;
    memcpy(b + 4, &page, 4);
    memcpy(b + 8, &min_app, 4);
    memcpy(b + 12, &max_app, 4);
    memcpy(b + 16, &km->affinity_mask, 4);
    memcpy(b + 20, &km->processor_count, 4);
    uint32_t type = 586;              /* PROCESSOR_INTEL_PENTIUM */
    memcpy(b + 24, &type, 4);
    memcpy(b + 28, &granularity, 4);
    /* wProcessorLevel = family, wProcessorRevision = model << 8 | stepping,
     * both from the diagnostic CPUID profile (leaf 1 EAX = 0x000306C4:
     * family 6, model 0x3C, stepping 4). */
    b[32] = 6; b[33] = 0;
    b[34] = 0x04; b[35] = 0x3C;
    rc = gm_write(km->mem, out, b, GKM_SYSTEM_INFO_SIZE);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}

static void memory_totals(uint64_t *total, uint64_t *available) {
    *total = 0;
    *available = 0;
    (void)gs_host_physical_bytes(total);
    long pages = sysconf(_SC_AVPHYS_PAGES);
    long size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && size > 0) *available = (uint64_t)pages * (uint64_t)size;
    if (!*total) *total = *available;
    if (*available > *total) *available = *total;
}

#define GKM_VIRTUAL_TOTAL 0x7FFF0000ull   /* 0x10000 .. 0x7FFEFFFF */

int gkm_global_memory_status(GuestKernelMisc *km, const uint32_t *args,
                             uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t out = args[0];
    if (!out) return GM_RANGE;
    int rc = gm_check(km->mem, out, GKM_MEMORYSTATUS_SIZE, GM_WRITE);
    if (rc) return rc;
    uint64_t total = 0, available = 0;
    memory_totals(&total, &available);
    uint32_t load = total ? (uint32_t)(((total - available) * 100ull) / total) : 0;
    uint8_t b[GKM_MEMORYSTATUS_SIZE];
    memset(b, 0, sizeof b);
    uint32_t length = GKM_MEMORYSTATUS_SIZE;
    memcpy(b + 0, &length, 4);
    memcpy(b + 4, &load, 4);
    uint32_t t32 = total > 0xffffffffull ? 0xffffffffu : (uint32_t)total;
    uint32_t a32 = available > 0xffffffffull ? 0xffffffffu : (uint32_t)available;
    memcpy(b + 8, &t32, 4);           /* dwTotalPhys */
    memcpy(b + 12, &a32, 4);          /* dwAvailPhys */
    /* No page file exists in this runtime, so the documented commit limit is
     * reported as the physical limit (deviation, not a fabricated number). */
    memcpy(b + 16, &t32, 4);          /* dwTotalPageFile */
    memcpy(b + 20, &a32, 4);          /* dwAvailPageFile */
    uint32_t vtotal = (uint32_t)GKM_VIRTUAL_TOTAL;
    memcpy(b + 24, &vtotal, 4);       /* dwTotalVirtual */
    memcpy(b + 28, &vtotal, 4);       /* dwAvailVirtual */
    rc = gm_write(km->mem, out, b, GKM_MEMORYSTATUS_SIZE);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}

int gkm_global_memory_status_ex(GuestKernelMisc *km, const uint32_t *args,
                                uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t out = args[0];
    if (!out) return GM_RANGE;
    uint32_t length = 0;
    int rc = get_u32(km->mem, out, &length);
    if (rc) return rc;
    if (length != GKM_MEMORYSTATUSEX_SIZE) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0;
        return GM_OK;
    }
    rc = gm_check(km->mem, out, GKM_MEMORYSTATUSEX_SIZE, GM_WRITE);
    if (rc) return rc;
    uint64_t total = 0, available = 0;
    memory_totals(&total, &available);
    uint32_t load = total ? (uint32_t)(((total - available) * 100ull) / total) : 0;
    uint8_t b[GKM_MEMORYSTATUSEX_SIZE];
    memset(b, 0, sizeof b);
    memcpy(b + 0, &length, 4);
    memcpy(b + 4, &load, 4);
    /* ullTotalPhys(8) ullAvailPhys(16) ullTotalPageFile(24)
     * ullAvailPageFile(32) ullTotalVirtual(40) ullAvailVirtual(48)
     * ullAvailExtendedVirtual(56, zero). */
    uint64_t values[6] = {total, available, total, available, GKM_VIRTUAL_TOTAL,
                          GKM_VIRTUAL_TOTAL};
    memcpy(b + 8, values, sizeof values);
    rc = gm_write(km->mem, out, b, GKM_MEMORYSTATUSEX_SIZE);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

/* --------------------------------------------------------------- version */

static int version_info_write(GuestKernelMisc *km, uint32_t out, int wide,
                              uint32_t size, uint32_t *result) {
    uint32_t expected = wide ? GKM_OSVERSIONINFOW_SIZE : GKM_OSVERSIONINFOA_SIZE;
    uint32_t expected_ex = wide ? GKM_OSVERSIONINFOEXW_SIZE
                                : GKM_OSVERSIONINFOEXA_SIZE;
    if (size != expected && size != expected_ex) {
        set_error(km, GKM_ERROR_INSUFFICIENT_BUFFER);
        *result = 0;
        return GM_OK;
    }
    int rc = gm_check(km->mem, out, size, GM_WRITE);
    if (rc) return rc;
    uint8_t b[GKM_OSVERSIONINFOEXW_SIZE];
    memset(b, 0, sizeof b);
    uint32_t fields[5] = {size, GKM_OS_MAJOR, GKM_OS_MINOR, GKM_OS_BUILD,
                          GKM_OS_PLATFORM};
    memcpy(b, fields, sizeof fields);
    /* szCSDVersion is an empty string (no service pack is modelled). */
    if (size == expected_ex) {
        size_t base = wide ? 276u : 148u;
        /* wServicePackMajor, wServicePackMinor, wSuiteMask, wProductType,
         * wReserved -- all zero except the product type at base + 6. */
        b[base + 6] = GKM_OS_PRODUCT_TYPE;
    }
    rc = gm_write(km->mem, out, b, size);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_get_version_ex_a(GuestKernelMisc *km, const uint32_t *args,
                         uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t out = args[0];
    if (!out) return GM_RANGE;
    uint32_t size = 0;
    int rc = get_u32(km->mem, out, &size);
    if (rc) return rc;
    return version_info_write(km, out, 0, size, result);
}

int gkm_get_version_ex_w(GuestKernelMisc *km, const uint32_t *args,
                         uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t out = args[0];
    if (!out) return GM_RANGE;
    uint32_t size = 0;
    int rc = get_u32(km->mem, out, &size);
    if (rc) return rc;
    return version_info_write(km, out, 1, size, result);
}

/* One field of the version projection; 0 when the type bit is not modelled. */
static int version_field(uint32_t type, uint32_t *value) {
    switch (type) {
    case GKM_VER_MAJORVERSION: *value = GKM_OS_MAJOR; return 1;
    case GKM_VER_MINORVERSION: *value = GKM_OS_MINOR; return 1;
    case GKM_VER_BUILDNUMBER: *value = GKM_OS_BUILD; return 1;
    case GKM_VER_PLATFORMID: *value = GKM_OS_PLATFORM; return 1;
    case GKM_VER_SERVICEPACKMAJOR: *value = 0; return 1;
    case GKM_VER_SERVICEPACKMINOR: *value = 0; return 1;
    case GKM_VER_SUITENAME: *value = 0; return 1;
    case GKM_VER_PRODUCT_TYPE: *value = GKM_OS_PRODUCT_TYPE; return 1;
    default: return 0;
    }
}

static int condition_holds(uint32_t condition, uint32_t actual,
                           uint32_t wanted) {
    switch (condition) {
    case GKM_VER_EQUAL: return actual == wanted;
    case GKM_VER_GREATER: return actual > wanted;
    case GKM_VER_GREATER_EQUAL: return actual >= wanted;
    case GKM_VER_LESS: return actual < wanted;
    case GKM_VER_LESS_EQUAL: return actual <= wanted;
    case GKM_VER_AND: return (actual & wanted) == wanted;
    case GKM_VER_OR: return (actual & wanted) != 0;
    default: return -1;
    }
}

static int verify_version(GuestKernelMisc *km, const uint32_t *args, int wide,
                          uint32_t *result) {
    uint32_t info = args[0], type_mask = args[1];
    uint64_t condition_mask = (uint64_t)args[2] | ((uint64_t)args[3] << 32);
    if (!info) return GM_RANGE;
    if (type_mask & ~GKM_VER_SUPPORTED_MASK) return GM_UNSUPPORTED;
    uint32_t size = wide ? GKM_OSVERSIONINFOEXW_SIZE : GKM_OSVERSIONINFOEXA_SIZE;
    int rc = gm_check(km->mem, info, size, GM_READ);
    if (rc) return rc;
    uint32_t fields[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < 4; i++) {
        rc = get_u32(km->mem, info + i * 4, &fields[i]);
        if (rc) return rc;
    }
    uint32_t extra = 0;
    size_t base = wide ? 276u : 148u;
    rc = get_u32(km->mem, info + (uint32_t)base, &extra);
    if (rc) return rc;
    /* wServicePackMajor is the low half of the dword at base, wServicePackMinor
     * the high half; wSuiteMask and wProductType follow. */
    uint32_t sp_major = extra & 0xffffu;
    uint32_t sp_minor = extra >> 16;
    uint32_t suite = 0, product = 0;
    rc = get_u32(km->mem, info + (uint32_t)base + 4, &suite);
    if (rc) return rc;
    suite &= 0xffffu;
    rc = get_u32(km->mem, info + (uint32_t)base + 4, &product);
    if (rc) return rc;
    product = (product >> 16) & 0xffu;

    /* Requested values come from the caller's structure, in the documented
     * field order: major, minor, build, platform, then the Ex fields. */
    uint32_t wanted_major = fields[1], wanted_minor = fields[2];
    uint32_t wanted_build = fields[3];
    uint32_t wanted_platform = 0;
    rc = get_u32(km->mem, info + 16, &wanted_platform);
    if (rc) return rc;
    uint32_t wanted[8] = {wanted_minor, wanted_major, wanted_build,
                          wanted_platform, sp_minor, sp_major, suite, product};
    uint32_t types[8] = {GKM_VER_MINORVERSION, GKM_VER_MAJORVERSION,
                         GKM_VER_BUILDNUMBER, GKM_VER_PLATFORMID,
                         GKM_VER_SERVICEPACKMINOR, GKM_VER_SERVICEPACKMAJOR,
                         GKM_VER_SUITENAME, GKM_VER_PRODUCT_TYPE};
    int ok = 1;
    for (unsigned i = 0; i < 8; i++) {
        if (!(type_mask & types[i])) continue;
        uint32_t actual = 0;
        if (!version_field(types[i], &actual)) return GM_UNSUPPORTED;
        uint32_t condition = (uint32_t)((condition_mask >> (i * 3)) & 0x7u);
        if (!condition) {
            /* A zero condition field means "not requested" for this type. */
            continue;
        }
        int hold = condition_holds(condition, actual, wanted[i]);
        if (hold < 0) return GM_UNSUPPORTED;
        if (!hold) ok = 0;
    }
    if (!ok) {
        set_error(km, GKM_ERROR_OLD_WIN_VERSION);
        *result = 0;
        return GM_OK;
    }
    *result = 1;
    return GM_OK;
}

int gkm_verify_version_info_a(GuestKernelMisc *km, const uint32_t *args,
                              uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    return verify_version(km, args, 0, result);
}

int gkm_verify_version_info_w(GuestKernelMisc *km, const uint32_t *args,
                              uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    return verify_version(km, args, 1, result);
}

int gkm_ver_set_condition_mask(GuestKernelMisc *km, const uint32_t *args,
                               uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint64_t mask = (uint64_t)args[0] | ((uint64_t)args[1] << 32);
    uint32_t type_mask = args[2];
    uint32_t condition = args[3] & 0x7u;
    if (type_mask & ~GKM_VER_SUPPORTED_MASK) return GM_UNSUPPORTED;
    if (!condition) return GM_UNSUPPORTED;
    for (unsigned bit = 0; bit < 8; bit++) {
        if (!(type_mask & (1u << bit))) continue;
        mask &= ~(0x7ull << (bit * 3));
        mask |= (uint64_t)condition << (bit * 3);
    }
    km->edx = (uint32_t)(mask >> 32);
    km->edx_valid = 1;
    *result = (uint32_t)mask;
    return GM_OK;
}

int gkm_take_edx(GuestKernelMisc *km, uint32_t *edx) {
    if (!km || !km->edx_valid) return 0;
    km->edx_valid = 0;
    if (edx) *edx = km->edx;
    return 1;
}

/* --------------------------------------------------------------- identity */

int gkm_get_computer_name_a(GuestKernelMisc *km, const uint32_t *args,
                            uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t buffer = args[0], size = args[1];
    if (!size) return GM_RANGE;
    uint32_t needed = 0;
    int rc = get_u32(km->mem, size, &needed);
    if (rc) return rc;
    size_t len = strlen(km->computer);
    if (!buffer || needed < len + 1) {
        rc = put_u32(km->mem, size, (uint32_t)len + 1);
        if (rc) return rc;
        set_error(km, GKM_ERROR_INSUFFICIENT_BUFFER);
        *result = 0;
        return GM_OK;
    }
    rc = write_a(km, buffer, km->computer, len + 1);
    if (rc) return rc;
    rc = put_u32(km->mem, size, (uint32_t)len);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_get_computer_name_w(GuestKernelMisc *km, const uint32_t *args,
                            uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t buffer = args[0], size = args[1];
    if (!size) return GM_RANGE;
    uint32_t needed = 0;
    int rc = get_u32(km->mem, size, &needed);
    if (rc) return rc;
    uint16_t wide[64];
    size_t len = 0;
    rc = ascii_to_wide(km->computer, wide, sizeof wide / sizeof wide[0], &len);
    if (rc) return rc;
    if (!buffer || needed < len + 1) {
        rc = put_u32(km->mem, size, (uint32_t)len + 1);
        if (rc) return rc;
        set_error(km, GKM_ERROR_INSUFFICIENT_BUFFER);
        *result = 0;
        return GM_OK;
    }
    rc = write_w(km, buffer, wide, len + 1);
    if (rc) return rc;
    rc = put_u32(km->mem, size, (uint32_t)len);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_get_user_name_a(GuestKernelMisc *km, const uint32_t *args,
                        uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t buffer = args[0], size = args[1];
    if (!size) return GM_RANGE;
    uint32_t needed = 0;
    int rc = get_u32(km->mem, size, &needed);
    if (rc) return rc;
    size_t len = strlen(km->user);
    if (!buffer || needed < len + 1) {
        rc = put_u32(km->mem, size, (uint32_t)len + 1);
        if (rc) return rc;
        set_error(km, GKM_ERROR_INSUFFICIENT_BUFFER);
        *result = 0;
        return GM_OK;
    }
    rc = write_a(km, buffer, km->user, len + 1);
    if (rc) return rc;
    rc = put_u32(km->mem, size, (uint32_t)len);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_get_user_name_w(GuestKernelMisc *km, const uint32_t *args,
                        uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t buffer = args[0], size = args[1];
    if (!size) return GM_RANGE;
    uint32_t needed = 0;
    int rc = get_u32(km->mem, size, &needed);
    if (rc) return rc;
    uint16_t wide[64];
    size_t len = 0;
    rc = ascii_to_wide(km->user, wide, sizeof wide / sizeof wide[0], &len);
    if (rc) return rc;
    if (!buffer || needed < len + 1) {
        rc = put_u32(km->mem, size, (uint32_t)len + 1);
        if (rc) return rc;
        set_error(km, GKM_ERROR_INSUFFICIENT_BUFFER);
        *result = 0;
        return GM_OK;
    }
    rc = write_w(km, buffer, wide, len + 1);
    if (rc) return rc;
    rc = put_u32(km->mem, size, (uint32_t)len);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_set_identity(GuestKernelMisc *km, const char *computer,
                     const char *user) {
    if (!km) return GM_RANGE;
    if (computer) {
        if (strlen(computer) >= sizeof km->computer) return GM_RANGE;
        snprintf(km->computer, sizeof km->computer, "%s", computer);
    }
    if (user) {
        if (strlen(user) >= sizeof km->user) return GM_RANGE;
        snprintf(km->user, sizeof km->user, "%s", user);
    }
    return GM_OK;
}

/* ------------------------------------------------------ small kernel bits */

int gkm_mul_div(GuestKernelMisc *km, const uint32_t *args, uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    int32_t number = (int32_t)args[0], numerator = (int32_t)args[1];
    int32_t denominator = (int32_t)args[2];
    if (!denominator) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0xffffffffu;
        return GM_OK;
    }
    int64_t product = (int64_t)number * (int64_t)numerator;
    /* Documented: the result is rounded to the nearest integer. */
    int64_t half = denominator / 2;
    int64_t quotient = product >= 0 ? (product + half) / denominator
                                    : (product - half) / denominator;
    if (quotient > INT32_MAX || quotient < INT32_MIN) {
        set_error(km, GKM_ERROR_ARITHMETIC_OVERFLOW);
        *result = 0xffffffffu;
        return GM_OK;
    }
    *result = (uint32_t)(int32_t)quotient;
    return GM_OK;
}

int gkm_is_debugger_present(GuestKernelMisc *km, const uint32_t *args,
                            uint32_t *result) {
    (void)args;
    if (!km) return GM_UNSUPPORTED;
    /* The real state: no debugger is attached to this guest. */
    *result = 0;
    return GM_OK;
}

int gkm_is_wow64_process(GuestKernelMisc *km, const uint32_t *args,
                         uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t handle = args[0], out = args[1];
    if (!out) return GM_RANGE;
    if (handle != 0xffffffffu && handle != 0x1f00u) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    /* The guest is 32-bit x86 on a modelled 32-bit Windows: not WOW64. */
    int rc = put_u32(km->mem, out, 0);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_get_process_affinity_mask(GuestKernelMisc *km, const uint32_t *args,
                                  uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t handle = args[0], process_mask = args[1], system_mask = args[2];
    if (handle != 0xffffffffu && handle != 0x1f00u) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    if (!process_mask || !system_mask) return GM_RANGE;
    int rc = put_u32(km->mem, process_mask, km->affinity_mask);
    if (rc) return rc;
    rc = put_u32(km->mem, system_mask, km->affinity_mask);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_get_logical_drives(GuestKernelMisc *km, const uint32_t *args,
                           uint32_t *result) {
    (void)args;
    if (!km) return GM_UNSUPPORTED;
    /* The path projection knows exactly one drive letter: C:. */
    *result = 0x00000004u;
    return GM_OK;
}

int gkm_get_disk_free_space_a(GuestKernelMisc *km, const uint32_t *args,
                              uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!km->map) return GM_UNSUPPORTED;
    char host[GPM_MAX_HOST];
    int rc = gpm_map(km->map, "C:\\Users\\Player", host, sizeof host);
    if (rc) return GM_UNSUPPORTED;
    struct statvfs st;
    if (statvfs(host, &st) != 0) {
        set_error(km, GKM_ERROR_PATH_NOT_FOUND);
        *result = 0;
        return GM_OK;
    }
    uint32_t sectors_per_cluster = 1;
    uint32_t bytes_per_sector = 512;
    uint64_t cluster_bytes = (uint64_t)st.f_frsize ? st.f_frsize
                                                   : (uint64_t)st.f_bsize;
    if (cluster_bytes) {
        sectors_per_cluster = (uint32_t)(cluster_bytes / bytes_per_sector);
        if (!sectors_per_cluster) sectors_per_cluster = 1;
    }
    uint64_t clusters = st.f_blocks;
    uint64_t free_clusters = st.f_bavail;
    uint32_t out[4] = {sectors_per_cluster, bytes_per_sector,
                       clusters > 0xffffffffull ? 0xffffffffu : (uint32_t)clusters,
                       free_clusters > 0xffffffffull ? 0xffffffffu
                                                     : (uint32_t)free_clusters};
    for (unsigned i = 0; i < 4; i++) {
        if (!args[i]) continue;
        rc = put_u32(km->mem, args[i], out[i]);
        if (rc) return rc;
    }
    *result = 1;
    return GM_OK;
}

int gkm_get_temp_path_w(GuestKernelMisc *km, const uint32_t *args,
                        uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    static const char *temp = "C:\\Users\\Player\\AppData\\Local\\Temp\\";
    uint32_t buffer = args[0], capacity = args[1];
    size_t len = strlen(temp);
    if (capacity < len + 1) {
        *result = (uint32_t)len + 1;
        return GM_OK;
    }
    if (!buffer) return GM_RANGE;
    uint16_t wide[GKM_MAX_PATH];
    size_t wlen = 0;
    int rc = ascii_to_wide(temp, wide, sizeof wide / sizeof wide[0], &wlen);
    if (rc) return rc;
    rc = write_w(km, buffer, wide, wlen + 1);
    if (rc) return rc;
    /* The directory is part of the path projection; create it so a caller that
     * writes a temp file finds it. A failure here is not an API error (the
     * documented contract only fills the buffer). */
    if (km->map) {
        char host[GPM_MAX_HOST];
        char guest[GKM_MAX_PATH];
        snprintf(guest, sizeof guest, "C:\\Users\\Player\\AppData\\Local\\Temp");
        if (!gpm_map(km->map, guest, host, sizeof host)) (void)mkdir(host, 0777);
    }
    *result = (uint32_t)wlen;
    return GM_OK;
}

int gkm_get_windows_directory_w(GuestKernelMisc *km, const uint32_t *args,
                                uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    static const char *windows = "C:\\Windows";
    uint32_t buffer = args[0], capacity = args[1];
    size_t len = strlen(windows);
    if (capacity < len + 1) {
        *result = (uint32_t)len + 1;
        return GM_OK;
    }
    if (!buffer) return GM_RANGE;
    uint16_t wide[GKM_MAX_PATH];
    size_t wlen = 0;
    int rc = ascii_to_wide(windows, wide, sizeof wide / sizeof wide[0], &wlen);
    if (rc) return rc;
    rc = write_w(km, buffer, wide, wlen + 1);
    if (rc) return rc;
    *result = (uint32_t)wlen;
    return GM_OK;
}

/* No 8.3 short names exist in the path projection, so the long form of a path
 * is the path itself -- the documented behaviour for a name without short
 * components. */
int gkm_get_long_path_name_w(GuestKernelMisc *km, const uint32_t *args,
                             uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint16_t text[GKM_MAX_PATH];
    size_t len = 0;
    int rc = read_w(km, args[0], text, sizeof text / sizeof text[0], &len);
    if (rc) return rc;
    uint32_t buffer = args[1], capacity = args[2];
    if (capacity < len + 1) {
        *result = (uint32_t)len + 1;
        return GM_OK;
    }
    if (!buffer) return GM_RANGE;
    rc = write_w(km, buffer, text, len + 1);
    if (rc) return rc;
    *result = (uint32_t)len;
    return GM_OK;
}

int gkm_set_end_of_file(GuestKernelMisc *km, const uint32_t *args,
                        uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t handle = args[0];
    GkmFilePrefix *file = (GkmFilePrefix *)gh_lookup(handle, GH_KIND_FILE, NULL);
    if (!file || file->fd < 0) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    off_t position = lseek(file->fd, 0, SEEK_CUR);
    if (position < 0) {
        set_error(km, GKM_ERROR_ACCESS_DENIED);
        *result = 0;
        return GM_OK;
    }
    if (ftruncate(file->fd, position) != 0) {
        set_error(km, GKM_ERROR_ACCESS_DENIED);
        *result = 0;
        return GM_OK;
    }
    *result = 1;
    return GM_OK;
}

int gkm_move_file_w(GuestKernelMisc *km, const uint32_t *args,
                    uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!km->map) return GM_UNSUPPORTED;
    char source[GKM_MAX_PATH], dest[GKM_MAX_PATH];
    char source_host[GPM_MAX_HOST], dest_host[GPM_MAX_HOST];
    int rc = read_w_ascii(km, args[0], source, sizeof source, NULL);
    if (rc) return rc;
    rc = read_w_ascii(km, args[1], dest, sizeof dest, NULL);
    if (rc) return rc;
    rc = gpm_resolve(km->map, source, GPM_MODE_READ, source_host,
                     sizeof source_host);
    if (rc == GPM_NOT_FOUND || rc == GPM_PATH_NOT_FOUND) {
        set_error(km, rc == GPM_NOT_FOUND ? GKM_ERROR_FILE_NOT_FOUND
                                         : GKM_ERROR_PATH_NOT_FOUND);
        *result = 0;
        return GM_OK;
    }
    if (rc) return GM_UNSUPPORTED;
    rc = gpm_resolve(km->map, dest, GPM_MODE_WRITE, dest_host, sizeof dest_host);
    if (rc == GPM_NOT_FOUND || rc == GPM_PATH_NOT_FOUND) {
        set_error(km, GKM_ERROR_PATH_NOT_FOUND);
        *result = 0;
        return GM_OK;
    }
    if (rc) return GM_UNSUPPORTED;
    if (rename(source_host, dest_host) != 0) {
        set_error(km, errno == EEXIST ? GKM_ERROR_ALREADY_EXISTS
                                      : GKM_ERROR_ACCESS_DENIED);
        *result = 0;
        return GM_OK;
    }
    *result = 1;
    return GM_OK;
}

int gkm_remove_directory_w(GuestKernelMisc *km, const uint32_t *args,
                           uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!km->map) return GM_UNSUPPORTED;
    char guest[GKM_MAX_PATH], host[GPM_MAX_HOST];
    int rc = read_w_ascii(km, args[0], guest, sizeof guest, NULL);
    if (rc) return rc;
    rc = gpm_resolve(km->map, guest, GPM_MODE_DIR, host, sizeof host);
    if (rc == GPM_NOT_FOUND || rc == GPM_PATH_NOT_FOUND) {
        set_error(km, GKM_ERROR_PATH_NOT_FOUND);
        *result = 0;
        return GM_OK;
    }
    if (rc) return GM_UNSUPPORTED;
    if (rmdir(host) != 0) {
        set_error(km, errno == ENOTEMPTY ? GKM_ERROR_DIR_NOT_EMPTY
                                         : GKM_ERROR_ACCESS_DENIED);
        *result = 0;
        return GM_OK;
    }
    *result = 1;
    return GM_OK;
}

/* ---------------------------------------------------- environment overlay */

static GkmEnv *env_find(GuestKernelMisc *km, const char *name) {
    for (uint32_t i = 0; i < km->env_overrides; i++)
        if (!strcmp(km->env_overlay[i].name, name)) return &km->env_overlay[i];
    return NULL;
}

static int env_set(GuestKernelMisc *km, const char *name, const char *value) {
    GkmEnv *slot = env_find(km, name);
    if (!slot) {
        if (km->env_overrides >= GKM_MAX_ENV) return GM_UNSUPPORTED;
        slot = &km->env_overlay[km->env_overrides];
        memset(slot, 0, sizeof *slot);
        slot->name = strdup(name);
        if (!slot->name) return GM_ALLOCATION;
        km->env_overrides++;
    }
    free(slot->value);
    slot->value = NULL;
    slot->deleted = 0;
    if (!value) {
        slot->deleted = 1;
        return GM_OK;
    }
    slot->value = strdup(value);
    if (!slot->value) return GM_ALLOCATION;
    return GM_OK;
}

int gkm_set_environment_variable_a(GuestKernelMisc *km, const uint32_t *args,
                                   uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    char name[GKM_MAX_NAME], value[GKM_MAX_STRING];
    int rc = read_a(km, args[0], name, sizeof name, NULL);
    if (rc) return rc;
    if (!name[0] || strchr(name, '=')) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0;
        return GM_OK;
    }
    if (args[1]) {
        rc = read_a(km, args[1], value, sizeof value, NULL);
        if (rc) return rc;
        rc = env_set(km, name, value);
    } else {
        rc = env_set(km, name, NULL);
    }
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_set_environment_variable_w(GuestKernelMisc *km, const uint32_t *args,
                                   uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint16_t wide[GKM_MAX_WSTRING];
    size_t len = 0;
    int rc = read_w(km, args[0], wide, sizeof wide / sizeof wide[0], &len);
    if (rc) return rc;
    char name[GKM_MAX_NAME];
    if (len + 1 > sizeof name) return GM_UNSUPPORTED;
    for (size_t i = 0; i <= len; i++) {
        if (wide[i] > 0x7f) return GM_UNSUPPORTED;
        name[i] = (char)wide[i];
    }
    if (!name[0] || strchr(name, '=')) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0;
        return GM_OK;
    }
    if (args[1]) {
        size_t vlen = 0;
        rc = read_w(km, args[1], wide, sizeof wide / sizeof wide[0], &vlen);
        if (rc) return rc;
        char value[GKM_MAX_STRING];
        if (vlen + 1 > sizeof value) return GM_UNSUPPORTED;
        for (size_t i = 0; i <= vlen; i++) {
            if (wide[i] > 0x7f) return GM_UNSUPPORTED;
            value[i] = (char)wide[i];
        }
        rc = env_set(km, name, value);
    } else {
        rc = env_set(km, name, NULL);
    }
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

/* The overlay in front of the read-only process profile. Semantics are the
 * documented GetEnvironmentVariableA ones: return the copied length without the
 * terminator; on a short buffer return the required size including the
 * terminator and set ERROR_INSUFFICIENT_BUFFER; a missing variable returns 0
 * with ERROR_ENVVAR_NOT_FOUND (203). */
int gkm_get_env_a(GuestKernelMisc *km, uint32_t name, uint32_t buffer,
                  uint32_t capacity, uint32_t *result, uint32_t *error) {
    if (!km) return GM_UNSUPPORTED;
    char key[GKM_MAX_NAME];
    int rc = read_a(km, name, key, sizeof key, NULL);
    if (rc) return rc;
    GkmEnv *slot = env_find(km, key);
    if (slot) {
        if (slot->deleted) {
            *result = 0;
            *error = 203;
            return GM_OK;
        }
        size_t len = strlen(slot->value);
        if (capacity <= len) {
            *result = (uint32_t)len + 1;
            *error = GKM_ERROR_INSUFFICIENT_BUFFER;
            return GM_OK;
        }
        if (!buffer) return GM_RANGE;
        rc = write_a(km, buffer, slot->value, len + 1);
        if (rc) return rc;
        *result = (uint32_t)len;
        return GM_OK;
    }
    return ge_get_a(km->mem, km->env, km->env_count, name, buffer, capacity,
                    *error, result);
}

/* ----------------------------------------------------------- debug output */

int gkm_output_debug_string_a(GuestKernelMisc *km, const uint32_t *args,
                              uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    char text[GKM_MAX_STRING];
    int rc = read_a(km, args[0], text, sizeof text, NULL);
    if (rc) return rc;
    km->debug_strings++;
    log_line(km, text);
    fprintf(stderr, "PORT_DEBUG_STRING %s\n", text);
    *result = 0;
    return GM_OK;
}

int gkm_output_debug_string_w(GuestKernelMisc *km, const uint32_t *args,
                              uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint16_t wide[GKM_MAX_WSTRING];
    size_t len = 0;
    int rc = read_w(km, args[0], wide, sizeof wide / sizeof wide[0], &len);
    if (rc) return rc;
    char text[GKM_MAX_WSTRING];
    for (size_t i = 0; i <= len; i++) {
        if (wide[i] > 0x7f) return GM_UNSUPPORTED;
        text[i] = (char)wide[i];
    }
    km->debug_strings++;
    log_line(km, text);
    fprintf(stderr, "PORT_DEBUG_STRING %s\n", text);
    *result = 0;
    return GM_OK;
}

int gkm_get_user_preferred_ui_languages(GuestKernelMisc *km,
                                        const uint32_t *args,
                                        uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t flags = args[0], count_out = args[1], buffer = args[2],
             size_out = args[3];
    if (flags != 0x8u) return GM_UNSUPPORTED;   /* MUI_LANGUAGE_NAME */
    if (!size_out) return GM_RANGE;
    /* MULTI_SZ: "en-US\0\0" -- 7 UTF-16 units including the double NUL. */
    static const uint16_t languages[7] = {'e', 'n', '-', 'U', 'S', 0, 0};
    uint32_t required = 7;
    uint32_t capacity = 0;
    int rc = get_u32(km->mem, size_out, &capacity);
    if (rc) return rc;
    if (!buffer || capacity < required) {
        rc = put_u32(km->mem, size_out, required);
        if (rc) return rc;
        if (count_out) {
            rc = put_u32(km->mem, count_out, 1);
            if (rc) return rc;
        }
        *result = 1;
        return GM_OK;
    }
    rc = write_w(km, buffer, languages, required);
    if (rc) return rc;
    rc = put_u32(km->mem, size_out, required);
    if (rc) return rc;
    if (count_out) {
        rc = put_u32(km->mem, count_out, 1);
        if (rc) return rc;
    }
    *result = 1;
    return GM_OK;
}

/* ------------------------------------------------------ process lifecycle */

int gkm_exit_process(GuestKernelMisc *km, const uint32_t *args,
                     uint32_t *result) {
    (void)result;
    if (!km) return GM_UNSUPPORTED;
    km->exit_code = args[0];
    /* An ordered runtime stop that carries the exit code. The harness decides
     * what an ExitProcess means; the port never reports it as a normal return
     * of the translated entry point. */
    fprintf(stderr, "PORT_EXIT ExitProcess code=%u\n", args[0]);
    return GM_UNSUPPORTED;
}

int gkm_terminate_process(GuestKernelMisc *km, const uint32_t *args,
                          uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t handle = args[0], code = args[1];
    if (handle != 0xffffffffu && handle != 0x1f00u) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    km->exit_code = code;
    fprintf(stderr, "PORT_EXIT TerminateProcess code=%u\n", code);
    return GM_UNSUPPORTED;
}

int gkm_get_exit_code_process(GuestKernelMisc *km, const uint32_t *args,
                              uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t handle = args[0], out = args[1];
    if (handle != 0xffffffffu && handle != 0x1f00u) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    if (!out) return GM_RANGE;
    int rc = put_u32(km->mem, out, km->exit_code);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}

int gkm_raise_exception(GuestKernelMisc *km, const uint32_t *args,
                        uint32_t *result) {
    (void)result;
    if (!km) return GM_UNSUPPORTED;
    km->raise_count++;
    fprintf(stderr,
            "PORT_RAISE_EXCEPTION code=0x%08X flags=0x%08X arguments=%u\n",
            args[0], args[1], args[2]);
    /* No SEH unwinder exists in this runtime, so the exception is reported and
     * the port stops. There is no handler to search and nothing to unwind. */
    return GM_UNSUPPORTED;
}

int gkm_set_unhandled_exception_filter(GuestKernelMisc *km,
                                       const uint32_t *args,
                                       uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t previous = km->unhandled_filter;
    km->unhandled_filter = args[0];
    *result = previous;
    return GM_OK;
}

int gkm_unhandled_exception_filter(GuestKernelMisc *km, const uint32_t *args,
                                   uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!km->unhandled_filter) {
        /* Documented default: no filter installed -> EXCEPTION_EXECUTE_HANDLER,
         * which terminates the process. */
        *result = 1;
        return GM_OK;
    }
    /* Calling the guest filter needs a guest-call hook the static image does
     * not have; pretending it returned EXCEPTION_EXECUTE_HANDLER would skip
     * the guest's own handler. */
    (void)args;
    return GM_UNSUPPORTED;
}

/* ------------------------------------------------------- thread settings */

static int thread_handle_ok(uint32_t handle) {
    if (handle == 0xfffffffeu || handle == 0xffffffffu) return 1;
    return gh_lookup(handle, GH_KIND_THREAD, NULL) != NULL;
}

int gkm_set_thread_priority(GuestKernelMisc *km, const uint32_t *args,
                            uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t handle = args[0];
    int32_t priority = (int32_t)args[1];
    if (!thread_handle_ok(handle)) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    int valid = (priority >= -15 && priority <= -1) ||
                (priority >= 0 && priority <= 15);
    if (!valid) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0;
        return GM_OK;
    }
    km->thread_priority = priority;
    *result = 1;
    return GM_OK;
}

int gkm_get_thread_priority(GuestKernelMisc *km, const uint32_t *args,
                            uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!thread_handle_ok(args[0])) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0xffffffffu;   /* THREAD_PRIORITY_ERROR_RETURN */
        return GM_OK;
    }
    *result = (uint32_t)km->thread_priority;
    return GM_OK;
}

int gkm_set_thread_affinity_mask(GuestKernelMisc *km, const uint32_t *args,
                                 uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!thread_handle_ok(args[0])) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0;
        return GM_OK;
    }
    uint32_t mask = args[1];
    if (!mask || (mask & ~km->affinity_mask)) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0;
        return GM_OK;
    }
    uint32_t previous = km->thread_affinity;
    km->thread_affinity = mask;
    *result = previous;
    return GM_OK;
}

int gkm_set_thread_ideal_processor(GuestKernelMisc *km, const uint32_t *args,
                                   uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    if (!thread_handle_ok(args[0])) {
        set_error(km, GKM_ERROR_INVALID_HANDLE);
        *result = 0xffffffffu;
        return GM_OK;
    }
    uint32_t ideal = args[1];
    if (ideal != GKM_MAXIMUM_PROCESSORS && ideal >= km->processor_count) {
        set_error(km, GKM_ERROR_INVALID_PARAMETER);
        *result = 0xffffffffu;
        return GM_OK;
    }
    uint32_t previous = km->ideal_processor;
    km->ideal_processor = ideal;
    *result = previous;
    return GM_OK;
}

/* --------------------------------------------------------- CreateProcessW */

int gkm_create_process_w(GuestKernelMisc *km, const uint32_t *args,
                         uint32_t *result) {
    (void)args;
    if (!km) return GM_UNSUPPORTED;
    /* This runtime cannot start a child process. The documented failure is
     * returned (FALSE + ERROR_FILE_NOT_FOUND), exactly like the Python model's
     * --process-creation-refusal: no handle, no thread, no process. */
    fprintf(stderr, "PORT_REFUSE CreateProcessW (no child processes)\n");
    set_error(km, GKM_ERROR_FILE_NOT_FOUND);
    *result = 0;
    return GM_OK;
}

/* ------------------------------------------------------------ SLIST ops */

/* The PE32 SLIST_HEADER is 8 bytes and must be 8-byte aligned (Windows
 * requires MEMORY_ALLOCATION_ALIGNMENT). The whole guest CPU set is serialised
 * by the runtime's machine lock; the host spinlock additionally makes the
 * 8-byte read-modify-write atomic against any bridge running concurrently. */
static uint32_t gkm_slist_lock;

static void slist_acquire(void) {
    while (__atomic_exchange_n(&gkm_slist_lock, 1u, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&gkm_slist_lock, __ATOMIC_RELAXED)) { }
    }
}

static void slist_release(void) {
    __atomic_store_n(&gkm_slist_lock, 0u, __ATOMIC_RELEASE);
}

static int slist_read(GuestKernelMisc *km, uint32_t header, uint32_t *next,
                      uint32_t *depth, uint32_t *sequence) {
    if (header & 7u) return GM_UNSUPPORTED;
    uint8_t b[GKM_SLIST_HEADER_SIZE];
    int rc = gm_check(km->mem, header, GKM_SLIST_HEADER_SIZE, GM_READ);
    if (rc) return rc;
    rc = gm_read(km->mem, header, b, GKM_SLIST_HEADER_SIZE);
    if (rc) return rc;
    if (next) *next = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                      ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (depth) *depth = (uint32_t)b[4] | ((uint32_t)b[5] << 8);
    if (sequence) *sequence = (uint32_t)b[6] | ((uint32_t)b[7] << 8);
    return 0;
}

static int slist_write(GuestKernelMisc *km, uint32_t header, uint32_t next,
                       uint32_t depth, uint32_t sequence) {
    uint8_t b[GKM_SLIST_HEADER_SIZE];
    b[0] = (uint8_t)next; b[1] = (uint8_t)(next >> 8);
    b[2] = (uint8_t)(next >> 16); b[3] = (uint8_t)(next >> 24);
    b[4] = (uint8_t)depth; b[5] = (uint8_t)(depth >> 8);
    b[6] = (uint8_t)sequence; b[7] = (uint8_t)(sequence >> 8);
    return gm_write(km->mem, header, b, GKM_SLIST_HEADER_SIZE);
}

int gkm_interlocked_push_entry_slist(GuestKernelMisc *km, const uint32_t *args,
                                     uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t header = args[0], entry = args[1];
    if (!header) return GM_RANGE;
    if (entry) {
        int rc = gm_check(km->mem, entry, 4, GM_WRITE);
        if (rc) return rc;
    }
    slist_acquire();
    uint32_t next = 0, depth = 0, sequence = 0;
    int rc = slist_read(km, header, &next, &depth, &sequence);
    if (!rc && entry) rc = put_u32(km->mem, entry, next);
    if (!rc) {
        uint32_t new_depth = (depth + 1) & 0xffffu;
        rc = slist_write(km, header, entry, new_depth,
                         (sequence + 1) & 0xffffu);
    }
    slist_release();
    if (rc) return rc;
    *result = next;
    return GM_OK;
}

int gkm_interlocked_pop_entry_slist(GuestKernelMisc *km, const uint32_t *args,
                                    uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t header = args[0];
    if (!header) return GM_RANGE;
    slist_acquire();
    uint32_t next = 0, depth = 0, sequence = 0;
    int rc = slist_read(km, header, &next, &depth, &sequence);
    if (!rc && next) {
        uint32_t following = 0;
        rc = get_u32(km->mem, next, &following);
        if (!rc)
            rc = slist_write(km, header, following, (depth ? depth - 1 : 0) & 0xffffu,
                             (sequence + 1) & 0xffffu);
    }
    slist_release();
    if (rc) return rc;
    *result = next;
    return GM_OK;
}

int gkm_interlocked_flush_slist(GuestKernelMisc *km, const uint32_t *args,
                                uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t header = args[0];
    if (!header) return GM_RANGE;
    slist_acquire();
    uint32_t next = 0, depth = 0, sequence = 0;
    int rc = slist_read(km, header, &next, &depth, &sequence);
    if (!rc) rc = slist_write(km, header, 0, 0, (sequence + 1) & 0xffffu);
    slist_release();
    if (rc) return rc;
    *result = next;
    return GM_OK;
}

int gkm_query_depth_slist(GuestKernelMisc *km, const uint32_t *args,
                          uint32_t *result) {
    if (!km) return GM_UNSUPPORTED;
    uint32_t header = args[0];
    if (!header) return GM_RANGE;
    uint32_t depth = 0;
    int rc = slist_read(km, header, NULL, &depth, NULL);
    if (rc) return rc;
    *result = depth;
    return GM_OK;
}

/* ------------------------------------------------------------ lifecycle */

int gkm_create(GuestMemory *mem, GuestProcessHeap *heap, GuestPathMap *map,
               const GuestLocale *locale, const GuestStringCompare *strcmp_,
               const GuestEnvironmentEntry *env, uint32_t env_count,
               GuestKernelMisc **out) {
    if (!mem || !out) return GM_RANGE;
    GuestKernelMisc *km = (GuestKernelMisc *)calloc(1, sizeof *km);
    if (!km) return GM_ALLOCATION;
    km->mem = mem;
    km->heap = heap;
    km->map = map;
    km->locale = locale;
    km->strcmp = strcmp_;
    km->env = env;
    km->env_count = env_count;
    km->exit_code = GKM_STILL_ACTIVE;
    km->thread_priority = GKM_THREAD_PRIORITY_NORMAL;
    /* Processor count = the number of guest CPUs this runtime really runs:
     * ONE. The machine lock (runtime/static_threads.c) lets exactly one guest
     * thread execute at a time, so the guest lives on a uniprocessor and is
     * told so. Reporting the host's cores (as this did before) made a game
     * size its job system for eight CPUs and then spin-wait on workers that
     * could never run at the same time. PORT_GUEST_PROCESSORS overrides the
     * count for experiments (it does not add guest CPUs). */
    long cores = 1;
    const char *override = getenv("PORT_GUEST_PROCESSORS");
    if (override && *override) cores = strtol(override, NULL, 10);
    if (cores < 1) cores = 1;
    if (cores > 8) cores = 8;
    km->processor_count = (uint32_t)cores;
    km->affinity_mask = (1u << km->processor_count) - 1u;
    km->thread_affinity = km->affinity_mask;
    km->ideal_processor = GKM_MAXIMUM_PROCESSORS;
    /* Port-owned identity: the user name is the USERNAME entry of the process
     * environment profile (the same name the path projection uses), the
     * computer name is a port constant because no Windows capture exists. */
    snprintf(km->computer, sizeof km->computer, "BURNOUTPR");
    snprintf(km->user, sizeof km->user, "Player");
    for (uint32_t i = 0; i < env_count; i++) {
        if (env[i].name && !strcmp(env[i].name, "USERNAME") && env[i].value) {
            snprintf(km->user, sizeof km->user, "%s", env[i].value);
            break;
        }
    }
    *out = km;
    return GM_OK;
}

void gkm_destroy(GuestKernelMisc *km) {
    if (!km) return;
    for (uint32_t i = 0; i < km->env_overrides; i++) {
        free(km->env_overlay[i].name);
        free(km->env_overlay[i].value);
    }
    free(km);
}

void gkm_set_teb(GuestKernelMisc *km, uint32_t teb) {
    if (km) km->teb = teb;
}

uint32_t gkm_teb(const GuestKernelMisc *km) { return km ? km->teb : 0; }

uint32_t gkm_global_blocks(const GuestKernelMisc *km) {
    return km ? km->global_blocks : 0;
}
uint32_t gkm_moveable_handles(const GuestKernelMisc *km) {
    return km ? km->moveable_handles : 0;
}
uint32_t gkm_debug_strings(const GuestKernelMisc *km) {
    return km ? km->debug_strings : 0;
}
uint32_t gkm_raise_count(const GuestKernelMisc *km) {
    return km ? km->raise_count : 0;
}
uint32_t gkm_exit_code(const GuestKernelMisc *km) {
    return km ? km->exit_code : 0;
}
uint32_t gkm_unhandled_filter(const GuestKernelMisc *km) {
    return km ? km->unhandled_filter : 0;
}
uint32_t gkm_env_overrides(const GuestKernelMisc *km) {
    return km ? km->env_overrides : 0;
}
uint32_t gkm_processor_count(const GuestKernelMisc *km) {
    return km ? km->processor_count : 0;
}
int gkm_thread_priority(const GuestKernelMisc *km) {
    return km ? km->thread_priority : 0;
}
uint32_t gkm_affinity_mask(const GuestKernelMisc *km) {
    return km ? km->affinity_mask : 0;
}
uint32_t gkm_ideal_processor(const GuestKernelMisc *km) {
    return km ? km->ideal_processor : 0;
}
const char *gkm_last_log(const GuestKernelMisc *km) {
    return km ? km->last_log : "";
}

/* ------------------------------------------------------------ registration */

/* LocalAlloc/LocalFree stay with runtime/local_heap.c and OutputDebugStringA /
 * ExitProcess stay with the A14 rows in runtime/static_bridges.c: those already
 * exist and are tested. Registering a second row for the same name would only
 * be shadowed (sb_find returns the first match), so it is not done. */
const GuestKernelMiscExport kernel32_misc_exports[] = {
    { "kernel32.dll", "GlobalAlloc", 8, gkm_global_alloc },
    { "kernel32.dll", "GlobalFree", 4, gkm_global_free },
    { "kernel32.dll", "GlobalLock", 4, gkm_global_lock },
    { "kernel32.dll", "GlobalUnlock", 4, gkm_global_unlock },
    { "kernel32.dll", "GlobalSize", 4, gkm_global_size },
    { "kernel32.dll", "GlobalFlags", 4, gkm_global_flags },
    { "kernel32.dll", "GlobalReAlloc", 12, gkm_global_realloc },
    { "kernel32.dll", "LocalLock", 4, gkm_global_lock },
    { "kernel32.dll", "LocalUnlock", 4, gkm_global_unlock },
    { "kernel32.dll", "LocalSize", 4, gkm_global_size },
    { "kernel32.dll", "LocalFlags", 4, gkm_global_flags },
    { "kernel32.dll", "LocalReAlloc", 12, gkm_global_realloc },

    { "kernel32.dll", "lstrlenA", 4, gkm_lstrlen_a },
    { "kernel32.dll", "lstrlenW", 4, gkm_lstrlen_w },
    { "kernel32.dll", "lstrcpyA", 8, gkm_lstrcpy_a },
    { "kernel32.dll", "lstrcpyW", 8, gkm_lstrcpy_w },
    { "kernel32.dll", "lstrcpynA", 12, gkm_lstrcpyn_a },
    { "kernel32.dll", "lstrcpynW", 12, gkm_lstrcpyn_w },
    { "kernel32.dll", "lstrcatA", 8, gkm_lstrcat_a },
    { "kernel32.dll", "lstrcatW", 8, gkm_lstrcat_w },
    { "kernel32.dll", "lstrcmpA", 8, gkm_lstrcmp_a },
    { "kernel32.dll", "lstrcmpW", 8, gkm_lstrcmp_w },
    { "kernel32.dll", "lstrcmpiA", 8, gkm_lstrcmpi_a },

    { "kernel32.dll", "GetSystemInfo", 4, gkm_get_system_info },
    { "kernel32.dll", "GetNativeSystemInfo", 4, gkm_get_system_info },
    { "kernel32.dll", "GlobalMemoryStatus", 4, gkm_global_memory_status },
    { "kernel32.dll", "GlobalMemoryStatusEx", 4, gkm_global_memory_status_ex },
    { "kernel32.dll", "GetVersionExA", 4, gkm_get_version_ex_a },
    { "kernel32.dll", "GetVersionExW", 4, gkm_get_version_ex_w },
    { "kernel32.dll", "VerifyVersionInfoA", 16, gkm_verify_version_info_a },
    { "kernel32.dll", "VerifyVersionInfoW", 16, gkm_verify_version_info_w },
    { "kernel32.dll", "VerSetConditionMask", 16, gkm_ver_set_condition_mask },
    { "kernel32.dll", "MulDiv", 12, gkm_mul_div },
    { "kernel32.dll", "GetComputerNameA", 8, gkm_get_computer_name_a },
    { "kernel32.dll", "GetComputerNameW", 8, gkm_get_computer_name_w },
    { "kernel32.dll", "GetUserNameA", 8, gkm_get_user_name_a },
    { "kernel32.dll", "GetUserNameW", 8, gkm_get_user_name_w },
    { "kernel32.dll", "IsDebuggerPresent", 0, gkm_is_debugger_present },
    { "kernel32.dll", "IsWow64Process", 8, gkm_is_wow64_process },
    { "kernel32.dll", "GetProcessAffinityMask", 12, gkm_get_process_affinity_mask },
    { "kernel32.dll", "GetLogicalDrives", 0, gkm_get_logical_drives },
    { "kernel32.dll", "GetDiskFreeSpaceA", 20, gkm_get_disk_free_space_a },
    { "kernel32.dll", "GetTempPathW", 8, gkm_get_temp_path_w },
    { "kernel32.dll", "GetWindowsDirectoryW", 8, gkm_get_windows_directory_w },
    { "kernel32.dll", "GetLongPathNameW", 12, gkm_get_long_path_name_w },
    { "kernel32.dll", "SetEndOfFile", 4, gkm_set_end_of_file },
    { "kernel32.dll", "MoveFileW", 8, gkm_move_file_w },
    { "kernel32.dll", "RemoveDirectoryW", 4, gkm_remove_directory_w },
    { "kernel32.dll", "SetEnvironmentVariableA", 8, gkm_set_environment_variable_a },
    { "kernel32.dll", "SetEnvironmentVariableW", 8, gkm_set_environment_variable_w },
    { "kernel32.dll", "OutputDebugStringW", 4, gkm_output_debug_string_w },
    { "kernel32.dll", "GetUserPreferredUILanguages", 16,
      gkm_get_user_preferred_ui_languages },
    { "kernel32.dll", "TerminateProcess", 8, gkm_terminate_process },
    { "kernel32.dll", "GetExitCodeProcess", 8, gkm_get_exit_code_process },
    { "kernel32.dll", "RaiseException", 12, gkm_raise_exception },
    { "kernel32.dll", "SetUnhandledExceptionFilter", 4,
      gkm_set_unhandled_exception_filter },
    { "kernel32.dll", "UnhandledExceptionFilter", 4, gkm_unhandled_exception_filter },
    { "kernel32.dll", "SetThreadPriority", 8, gkm_set_thread_priority },
    { "kernel32.dll", "GetThreadPriority", 4, gkm_get_thread_priority },
    { "kernel32.dll", "SetThreadAffinityMask", 8, gkm_set_thread_affinity_mask },
    { "kernel32.dll", "SetThreadIdealProcessor", 8, gkm_set_thread_ideal_processor },
    { "kernel32.dll", "CreateProcessW", 40, gkm_create_process_w },
    { "kernel32.dll", "InterlockedPushEntrySList", 8,
      gkm_interlocked_push_entry_slist },
    { "kernel32.dll", "InterlockedPopEntrySList", 4,
      gkm_interlocked_pop_entry_slist },
    { "kernel32.dll", "InterlockedFlushSList", 4, gkm_interlocked_flush_slist },
    { "kernel32.dll", "QueryDepthSList", 4, gkm_query_depth_slist },
};
const uint32_t kernel32_misc_export_count =
    (uint32_t)(sizeof kernel32_misc_exports / sizeof kernel32_misc_exports[0]);
