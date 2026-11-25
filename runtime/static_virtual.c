/* VirtualAlloc / VirtualFree / VirtualQuery for the flat-memory runtime.
 *
 * Same API as runtime/guest_virtual_memory.c (so the bridges are unchanged),
 * different backing: the whole 32-bit guest space is one host mapping, so a
 * reservation is a bookkeeping entry and a committed page is simply a page of
 * that mapping.  Freshly committed pages read as zero: decommit/release hands
 * the pages back with madvise(MADV_DONTNEED), which is what makes the next
 * commit zero-filled again.
 *
 * Windows semantics kept: 64-KiB allocation granularity, 4-KiB pages, commit
 * inside a reservation, MEM_RELEASE of a whole reservation, MEM_DECOMMIT of a
 * page range, MEMORY_BASIC_INFORMATION (28 bytes).  The image is not
 * large-address-aware, so everything stays below 2 GiB.  Page protections are
 * recorded and reported but not enforced by the host mapping (PAGE_GUARD and
 * PAGE_NOACCESS as an access trap are therefore named stops, not silently
 * ignored).
 */
#include "guest_virtual_memory.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint8_t *port_flat_base(void);

typedef struct Region {
    uint32_t base, size, protect;       /* reservation, allocation protect */
    uint8_t *page;                      /* per page: 0 = reserved, else protect code (1..) */
    uint32_t *page_protect;             /* per committed page: Win32 protect value */
    struct Region *next;
} Region;

typedef struct { uint32_t base, limit, cursor; } Arena;

struct GuestVirtualMemory {
    GuestMemory *memory;
    Arena arena[4];
    unsigned arenas;
    Region *regions;                    /* sorted by base */
    pthread_mutex_t lock;
};

/* Regions this allocator does not own (image sections, stacks, the process
 * heap) are described by the runtime that mapped them. Returns 1 and fills
 * the seven MEMORY_BASIC_INFORMATION words when it knows the address. */
static int (*g_describe)(uint32_t address, uint32_t mbi[7]);
void gvm_set_region_describer(int (*fn)(uint32_t address, uint32_t mbi[7])) { g_describe = fn; }

static int last_error(GuestVirtualMemory *vm, uint32_t e) {
    (void)vm;
    extern __thread uint32_t g_teb;
    memcpy(port_flat_base() + g_teb + 0x34, &e, 4);
    return GM_OK;
}

GuestVirtualMemory *gvm_create(GuestMemory *memory, uint32_t base, uint32_t limit) {
    GuestVirtualMemory *vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;
    vm->memory = memory;
    /* The constructor range first; then the other free stretches of the 2-GiB
     * user space this runtime leaves unused (see the layout in static_entry.c). */
    vm->arena[0] = (Arena){ base, limit, base };
    vm->arena[1] = (Arena){ 0x50000000u, 0x5F000000u, 0x50000000u };   /* 0x31..0x3F is the thread-stack arena */
    vm->arena[2] = (Arena){ 0x20000000u, 0x30000000u, 0x20000000u };
    vm->arena[3] = (Arena){ 0x02000000u, 0x10000000u, 0x02000000u };
    vm->arenas = 4;
    pthread_mutex_init(&vm->lock, NULL);
    return vm;
}
void gvm_destroy(GuestVirtualMemory *vm) {
    if (!vm) return;
    for (Region *r = vm->regions; r;) { Region *n = r->next; free(r->page); free(r->page_protect); free(r); r = n; }
    free(vm);
}
static Region *find(GuestVirtualMemory *vm, uint32_t address) {
    for (Region *r = vm->regions; r; r = r->next)
        if (address >= r->base && address - r->base < r->size) return r;
    return NULL;
}
static int range_free(GuestVirtualMemory *vm, uint32_t base, uint32_t size) {
    for (Region *r = vm->regions; r; r = r->next)
        if (base < r->base + r->size && r->base < base + size) return 0;
    return 1;
}
static int in_arena(GuestVirtualMemory *vm, uint32_t base, uint32_t size) {
    for (unsigned i = 0; i < vm->arenas; i++)
        if (base >= vm->arena[i].base && (uint64_t)base + size <= vm->arena[i].limit) return 1;
    return 0;
}
static Region *reserve(GuestVirtualMemory *vm, uint32_t base, uint32_t size, uint32_t protect) {
    Region *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    uint32_t pages = size / GVM_PAGE;
    r->page = calloc(pages, 1);
    r->page_protect = calloc(pages, sizeof *r->page_protect);
    if (!r->page || !r->page_protect) { free(r->page); free(r->page_protect); free(r); return NULL; }
    r->base = base; r->size = size; r->protect = protect;
    Region **slot = &vm->regions;
    while (*slot && (*slot)->base < base) slot = &(*slot)->next;
    r->next = *slot; *slot = r;
    return r;
}
static uint32_t pick(GuestVirtualMemory *vm, uint32_t size) {
    for (unsigned i = 0; i < vm->arenas; i++) {
        Arena *a = &vm->arena[i];
        /* first fit from the cursor, then from the arena start (released holes) */
        for (int pass = 0; pass < 2; pass++) {
            uint32_t at = pass ? a->base : a->cursor;
            while ((uint64_t)at + size <= a->limit) {
                Region *hit = NULL;
                for (Region *r = vm->regions; r; r = r->next)
                    if (at < r->base + r->size && r->base < at + size) { hit = r; break; }
                if (!hit) { a->cursor = at + size; return at; }
                at = (hit->base + hit->size + GVM_GRANULARITY - 1) & ~(GVM_GRANULARITY - 1);
            }
        }
    }
    return 0;
}
static int protect_ok(uint32_t protect) {
    uint32_t base = protect & 0xFFu, mod = protect & ~0xFFu;
    if (mod & ~0x400u) return 0;                    /* PAGE_WRITECOMBINE only; GUARD/NOCACHE not modelled */
    return base == 0x01 || base == 0x02 || base == 0x04 || base == 0x08 ||
           base == 0x10 || base == 0x20 || base == 0x40 || base == 0x80;
}
int gvm_alloc(GuestVirtualMemory *vm, uint32_t address, uint32_t size, uint32_t type,
              uint32_t protect, uint32_t *result) {
    if (!vm || !result) return GM_UNSUPPORTED;
    *result = 0;
    if (!size || (type & ~(GVM_MEM_COMMIT | GVM_MEM_RESERVE | 0x00100000u /* MEM_TOP_DOWN: a placement hint */)) ||
        !(type & (GVM_MEM_COMMIT | GVM_MEM_RESERVE))) {
        if (type & ~(GVM_MEM_COMMIT | GVM_MEM_RESERVE | 0x00100000u)) {
            fprintf(stderr, "PORT_NOTE VirtualAlloc: allocation type 0x%X is not modelled\n", type);
            return GM_UNSUPPORTED;
        }
        return last_error(vm, GVM_ERROR_INVALID_PARAMETER);
    }
    if (!protect_ok(protect)) {
        fprintf(stderr, "PORT_NOTE VirtualAlloc: protection 0x%X is not modelled\n", protect);
        return GM_UNSUPPORTED;
    }
    pthread_mutex_lock(&vm->lock);
    int rc = GM_OK;
    Region *r = address ? find(vm, address) : NULL;
    uint32_t start, length, result_base;
    if (!r) {
        /* new reservation (MEM_COMMIT alone reserves too, as on Windows) */
        uint32_t base, span;
        if (address) {
            base = address & ~(GVM_GRANULARITY - 1);
            span = (address - base + size + GVM_PAGE - 1) & ~(GVM_PAGE - 1);
            if (!in_arena(vm, base, span) || !range_free(vm, base, span)) { rc = last_error(vm, GVM_ERROR_INVALID_ADDRESS); goto out; }
        } else {
            span = (size + GVM_PAGE - 1) & ~(GVM_PAGE - 1);
            if (span < size) { rc = last_error(vm, GVM_ERROR_NOT_ENOUGH_MEMORY); goto out; }
            base = pick(vm, (span + GVM_GRANULARITY - 1) & ~(GVM_GRANULARITY - 1));
            if (!base) { rc = last_error(vm, GVM_ERROR_NOT_ENOUGH_MEMORY); goto out; }
        }
        r = reserve(vm, base, span, protect);
        if (!r) { rc = last_error(vm, GVM_ERROR_NOT_ENOUGH_MEMORY); goto out; }
        start = address ? (address & ~(GVM_PAGE - 1)) : base;
        length = address ? ((address + size + GVM_PAGE - 1) & ~(GVM_PAGE - 1)) - start : span;
        result_base = (!address || (type & GVM_MEM_RESERVE)) ? base : start;
    } else {
        if (type & GVM_MEM_RESERVE && !(type & GVM_MEM_COMMIT)) { rc = last_error(vm, GVM_ERROR_INVALID_ADDRESS); goto out; }
        start = address & ~(GVM_PAGE - 1);
        length = ((address + size + GVM_PAGE - 1) & ~(GVM_PAGE - 1)) - start;
        if ((uint64_t)start + length > (uint64_t)r->base + r->size) { rc = last_error(vm, GVM_ERROR_INVALID_ADDRESS); goto out; }
        result_base = start;
    }
    if (type & GVM_MEM_COMMIT) {
        uint32_t first = (start - r->base) / GVM_PAGE, count = length / GVM_PAGE;
        for (uint32_t i = 0; i < count; i++) { r->page[first + i] = 1; r->page_protect[first + i] = protect; }
    }
    *result = result_base;
out:
    pthread_mutex_unlock(&vm->lock);
    return rc;
}
int gvm_free(GuestVirtualMemory *vm, uint32_t address, uint32_t size, uint32_t type, uint32_t *result) {
    if (!vm || !result) return GM_UNSUPPORTED;
    *result = 0;
    pthread_mutex_lock(&vm->lock);
    int rc = GM_OK;
    Region *r = find(vm, address);
    if (!r) { rc = last_error(vm, GVM_ERROR_INVALID_ADDRESS); goto out; }
    if (type == GVM_MEM_RELEASE) {
        if (size || address != r->base) { rc = last_error(vm, GVM_ERROR_INVALID_PARAMETER); goto out; }
        madvise(port_flat_base() + r->base, r->size, MADV_DONTNEED);
        Region **slot = &vm->regions;
        while (*slot != r) slot = &(*slot)->next;
        *slot = r->next;
        free(r->page); free(r->page_protect); free(r);
        *result = 1;
    } else if (type == GVM_MEM_DECOMMIT) {
        uint32_t start = address & ~(GVM_PAGE - 1);
        uint32_t length = size ? ((address + size + GVM_PAGE - 1) & ~(GVM_PAGE - 1)) - start : r->base + r->size - start;
        if (!size && address != r->base) { rc = last_error(vm, GVM_ERROR_INVALID_PARAMETER); goto out; }
        if ((uint64_t)start + length > (uint64_t)r->base + r->size) { rc = last_error(vm, GVM_ERROR_INVALID_ADDRESS); goto out; }
        uint32_t first = (start - r->base) / GVM_PAGE, count = length / GVM_PAGE;
        for (uint32_t i = 0; i < count; i++) { r->page[first + i] = 0; r->page_protect[first + i] = 0; }
        madvise(port_flat_base() + start, length, MADV_DONTNEED);
        *result = 1;
    } else rc = last_error(vm, GVM_ERROR_INVALID_PARAMETER);
out:
    pthread_mutex_unlock(&vm->lock);
    return rc;
}
int gvm_query(GuestVirtualMemory *vm, uint32_t address, uint32_t buffer, uint32_t length, uint32_t *result) {
    if (!vm || !result) return GM_UNSUPPORTED;
    *result = 0;
    if (length < GVM_MBI_SIZE) return GM_UNSUPPORTED;
    pthread_mutex_lock(&vm->lock);
    uint32_t page = address & ~(GVM_PAGE - 1), mbi[7];
    Region *r = find(vm, page);
    int rc = GM_OK;
    if (r) {
        uint32_t idx = (page - r->base) / GVM_PAGE, pages = r->size / GVM_PAGE, n = 1;
        uint8_t state = r->page[idx]; uint32_t prot = r->page_protect[idx];
        while (idx + n < pages && r->page[idx + n] == state && r->page_protect[idx + n] == prot) n++;
        mbi[0] = page; mbi[1] = r->base; mbi[2] = r->protect; mbi[3] = n * GVM_PAGE;
        mbi[4] = state ? GVM_MEM_COMMIT : GVM_MEM_RESERVE; mbi[5] = state ? prot : 0; mbi[6] = GVM_MEM_PRIVATE;
    } else {
        /* Outside the managed reservations this runtime only knows "not one of
         * mine": report it as free up to the next reservation. Image/stack/heap
         * queries are not modelled here. */
        if (!in_arena(vm, page, GVM_PAGE)) {
            pthread_mutex_unlock(&vm->lock);
            if (g_describe && g_describe(address, mbi)) {
                rc = gm_write(vm->memory, buffer, mbi, GVM_MBI_SIZE);
                if (rc) return rc;
                *result = GVM_MBI_SIZE;
                return GM_OK;
            }
            fprintf(stderr, "PORT_NOTE VirtualQuery(0x%08X): no mapping known at this address\n", address);
            return GM_UNSUPPORTED;
        }
        uint32_t end = 0;
        for (unsigned i = 0; i < vm->arenas; i++) if (page >= vm->arena[i].base && page < vm->arena[i].limit) end = vm->arena[i].limit;
        for (Region *q = vm->regions; q; q = q->next) if (q->base > page && q->base < end) end = q->base;
        mbi[0] = page; mbi[1] = 0; mbi[2] = 0; mbi[3] = end - page; mbi[4] = GVM_MEM_FREE; mbi[5] = GVM_PAGE_NOACCESS; mbi[6] = 0;
    }
    pthread_mutex_unlock(&vm->lock);
    rc = gm_write(vm->memory, buffer, mbi, GVM_MBI_SIZE);
    if (rc) return rc;
    *result = GVM_MBI_SIZE;
    return GM_OK;
}
uint32_t gvm_regions(GuestVirtualMemory *vm, uint32_t *quads, uint32_t capacity) {
    uint32_t n = 0;
    for (Region *r = vm ? vm->regions : NULL; r; r = r->next) {
        if (n < capacity) {
            uint32_t committed = 0;
            for (uint32_t i = 0; i < r->size / GVM_PAGE; i++) committed += r->page[i] ? GVM_PAGE : 0;
            quads[n * 4] = r->base; quads[n * 4 + 1] = r->size; quads[n * 4 + 2] = r->protect; quads[n * 4 + 3] = committed;
        }
        n++;
    }
    return n;
}
uint32_t gvm_commits(GuestVirtualMemory *vm, uint32_t *quads, uint32_t capacity) { (void)vm; (void)quads; (void)capacity; return 0; }

/* Export records (same names/argument bytes as guest_virtual_memory.c). */
const GvmExport gvm_exports[] = {
    { "kernel32.dll", "VirtualAlloc", 16, (void *)gvm_alloc },
    { "kernel32.dll", "VirtualFree", 12, (void *)gvm_free },
    { "kernel32.dll", "VirtualQuery", 12, (void *)gvm_query },
};
const uint32_t gvm_export_count = sizeof gvm_exports / sizeof gvm_exports[0];
