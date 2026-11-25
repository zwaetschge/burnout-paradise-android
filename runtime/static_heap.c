/* A14 -- the process heap of the *static* image.
 *
 * Why this file exists
 * --------------------
 * runtime/guest_process_heap.c models a Win32 heap by mapping freshly
 * calloc'd host buffers into a GuestMemory page table (`gm_map`). That is
 * exactly right for the Unicorn oracle harness, where every guest access goes
 * through gm_read/gm_write. It cannot work for the static lift: the lifted
 * code addresses guest memory as `g_mem_base + va` (a single flat 4 GiB
 * reservation), so a block that lives in a separate host buffer is invisible
 * to it, and `gm_map` refuses every address because the flat reservation
 * already owns all 4 GiB (GM_OVERLAP for every page, forever).
 *
 * This file therefore implements the *same* API (runtime/guest_process_heap.h)
 * as a bump/free-list allocator inside the flat reservation itself:
 *
 *   GPH_BASE 0x11000000 .. GPH_LIMIT 0x20000000   (240 MiB)
 *
 * Consequences, documented rather than hidden:
 *   * no per-block guest page protection (the flat model has none anyway);
 *   * no coalescing of adjacent free blocks -- 16-byte alignment and first-fit
 *     reuse keep the arena from fragmenting in practice, and the arena is
 *     240 MiB of a 4 GiB space;
 *   * every block is zeroed on allocation, so "fresh backing is deterministic
 *     zero" (the property guest_process_heap.c documents) still holds for
 *     reused blocks;
 *   * a block is only ever handed out once, so `gph_free` on a foreign pointer
 *     is still GM_UNSUPPORTED (a named stop), never a silent no-op.
 *
 * The header contract (handle validation, flag masks, *address=0 on
 * exhaustion, gph_regions triples) is byte-for-byte the same as
 * guest_process_heap.c; tools/test_guest_process_heap.py keeps testing the
 * original file, this one is exercised by the static image's own selftest.
 */
#include "guest_process_heap.h"
#include "static_entry.h"

#include <stdlib.h>
#include <string.h>

#define FH_ALIGN 16u

typedef struct FBlock {
    uint32_t address, capacity, requested;
    struct FBlock *next;
} FBlock;

typedef struct FFree {
    uint32_t address, capacity;
    struct FFree *next;
} FFree;

struct GuestProcessHeap {
    GuestMemory *memory;
    uint8_t *flat;
    FBlock *blocks;      /* live blocks, newest first */
    FFree *free_list;    /* reusable ranges, newest first */
    uint32_t next_address;
    uint32_t active;
    uint32_t live_count;
};

/* One process heap per guest process: the flat arena is process-wide, so a
 * second gph_create() (runtime/host_bridges.c's hb_initialize_state() asks for
 * one) must return the same heap instead of starting a second bump pointer at
 * GPH_BASE and handing out overlapping blocks. */
static GuestProcessHeap *g_flat_heap;

GuestProcessHeap *gph_create(GuestMemory *memory) {
    if (!memory) return NULL;
    if (g_flat_heap) return g_flat_heap;
    GuestProcessHeap *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->memory = memory;
    h->flat = port_flat_base();
    if (!h->flat) { free(h); return NULL; }
    h->next_address = GPH_BASE;
    g_flat_heap = h;
    return h;
}

void gph_destroy(GuestProcessHeap *h) {
    if (!h) return;
    /* The singleton outlives a single subsystem's teardown: dropping it would
     * invalidate every other holder of the same pointer. */
    if (h == g_flat_heap) return;
    FBlock *b = h->blocks;
    while (b) { FBlock *n = b->next; free(b); b = n; }
    FFree *f = h->free_list;
    while (f) { FFree *n = f->next; free(f); f = n; }
    free(h);
}

static int valid(GuestProcessHeap *h, uint32_t handle, uint32_t flags,
                 uint32_t allowed) {
    return (!h || !h->active || handle != GPH_HANDLE || (flags & ~allowed))
               ? GM_UNSUPPORTED
               : GM_OK;
}

int gph_initialize(GuestProcessHeap *h, uint32_t peb) {
    if (!h || (uint64_t)peb + 0x1c > UINT64_C(0x100000000)) return GM_RANGE;
    const uint8_t data[] = {0, 0, 0, 0x78};   /* 0x78000000 little endian */
    int rc = gm_write(h->memory, peb + 0x18, data, 4);
    if (!rc) h->active = 1;
    return rc;
}

int gph_current(GuestProcessHeap *h, uint32_t peb, uint32_t *handle) {
    if (!h || !handle || (uint64_t)peb + 0x1c > UINT64_C(0x100000000))
        return GM_RANGE;
    uint8_t b[4];
    int rc = gm_read(h->memory, peb + 0x18, b, 4);
    if (rc) return rc;
    uint32_t a = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if ((rc = valid(h, a, 0, 0))) return rc;
    *handle = a;
    return GM_OK;
}

static FBlock *find(GuestProcessHeap *h, uint32_t address) {
    for (FBlock *b = h->blocks; b; b = b->next)
        if (b->address == address) return b;
    return NULL;
}

/* Take a range from the free list (first fit) or bump the arena. */
static int take(GuestProcessHeap *h, uint32_t capacity, uint32_t *address) {
    for (FFree **slot = &h->free_list; *slot; slot = &(*slot)->next) {
        FFree *f = *slot;
        if (f->capacity < capacity) continue;
        uint32_t a = f->address;
        if (f->capacity > capacity) {
            f->address += capacity;
            f->capacity -= capacity;
        } else {
            *slot = f->next;
            free(f);
        }
        *address = a;
        return GM_OK;
    }
    uint32_t a = h->next_address;
    if ((uint64_t)a + capacity > GPH_LIMIT) { *address = 0; return GM_OK; }
    h->next_address = a + capacity;
    *address = a;
    return GM_OK;
}

int gph_alloc(GuestProcessHeap *h, uint32_t handle, uint32_t flags, uint32_t n,
              uint32_t *address) {
    int rc = valid(h, handle, flags, 8);
    if (rc) return rc;
    if (!address) return GM_RANGE;
    if (n > GPH_LIMIT - GPH_BASE) { *address = 0; return GM_OK; }
    uint32_t capacity = (n + (FH_ALIGN - 1)) & ~(FH_ALIGN - 1);
    if (!capacity) capacity = FH_ALIGN;
    uint32_t a = 0;
    rc = take(h, capacity, &a);
    if (rc) return rc;
    if (!a) { *address = 0; return GM_OK; }
    FBlock *b = calloc(1, sizeof *b);
    if (!b) { *address = 0; return GM_OK; }
    b->address = a;
    b->capacity = capacity;
    b->requested = n;
    b->next = h->blocks;
    h->blocks = b;
    h->live_count++;
    /* Deterministic zero, including a reused range. */
    memset(h->flat + a, 0, capacity);
    *address = a;
    return GM_OK;
}

int gph_free(GuestProcessHeap *h, uint32_t handle, uint32_t flags,
             uint32_t address) {
    int rc = valid(h, handle, flags, 0);
    if (rc) return rc;
    FBlock **slot = &h->blocks;
    while (*slot && (*slot)->address != address) slot = &(*slot)->next;
    if (!*slot) return GM_UNSUPPORTED;   /* foreign pointer: named stop */
    FBlock *b = *slot;
    *slot = b->next;
    FFree *f = calloc(1, sizeof *f);
    if (!f) return GM_ALLOCATION;
    f->address = b->address;
    f->capacity = b->capacity;
    f->next = h->free_list;
    h->free_list = f;
    h->live_count--;
    free(b);
    return GM_OK;
}

int gph_size(GuestProcessHeap *h, uint32_t handle, uint32_t flags,
             uint32_t address, uint32_t *size) {
    int rc = valid(h, handle, flags, 0);
    if (rc) return rc;
    if (!size) return GM_RANGE;
    FBlock *b = find(h, address);
    if (!b) return GM_UNSUPPORTED;
    *size = b->requested;
    return GM_OK;
}

int gph_realloc(GuestProcessHeap *h, uint32_t handle, uint32_t flags,
                uint32_t address, uint32_t n, uint32_t *result) {
    int rc = valid(h, handle, flags, 0x18);
    if (rc) return rc;
    if (!result) return GM_RANGE;
    FBlock *b = find(h, address);
    if (!b) return GM_UNSUPPORTED;
    if (n <= b->capacity) {
        if ((flags & 8) && n > b->requested)
            memset(h->flat + address + b->requested, 0, n - b->requested);
        b->requested = n;
        *result = address;
        return GM_OK;
    }
    if (flags & 0x10) { *result = 0; return GM_OK; }   /* in place only */
    uint32_t moved = 0;
    rc = gph_alloc(h, handle, flags & 8, n, &moved);
    if (rc || !moved) { if (!rc) *result = 0; return rc; }
    FBlock *replacement = find(h, moved);
    if (!replacement) return GM_UNSUPPORTED;
    memcpy(h->flat + moved, h->flat + address, b->requested);
    rc = gph_free(h, handle, 0, address);
    if (rc) { gph_free(h, handle, 0, moved); return rc; }
    *result = moved;
    return GM_OK;
}

uint32_t gph_regions(GuestProcessHeap *h, uint32_t *triples, uint32_t capacity) {
    uint32_t count = 0;
    for (FBlock *b = h ? h->blocks : NULL; b; b = b->next) {
        if (triples && count < capacity) {
            triples[count * 3] = b->address;
            triples[count * 3 + 1] = b->capacity;
            triples[count * 3 + 2] = b->requested;
        }
        count++;
    }
    return count;
}

uint32_t gph_live_blocks(GuestProcessHeap *h) { return h ? h->live_count : 0; }
