/* Local heap for the flat-memory runtimes (fast / PF engine).
 *
 * Same API and the same address policy as runtime/local_heap.c (first block at
 * 0x10000000, page-rounded bump allocation, reset when the heap is empty), so a
 * guest sees exactly the pointers the proven model hands out.  The difference
 * is only the backing: in the flat runtime the whole 4-GiB guest space is one
 * mapping, so a block is a zero-filled range of it rather than a separately
 * mapped host buffer (gm_map there always reports GM_OVERLAP).
 */
#include "local_heap.h"
#include <stdlib.h>
#include <string.h>
typedef struct Block { uint32_t address, size; struct Block *next; } Block;
struct GuestLocalHeap { GuestMemory *memory; uint32_t next_address; Block *blocks; };

GuestLocalHeap *lh_create(GuestMemory *memory) {
    GuestLocalHeap *h = calloc(1, sizeof(*h));
    if (h) { h->memory = memory; h->next_address = 0x10000000; }
    return h;
}
void lh_destroy(GuestLocalHeap *h) {
    if (!h) return;
    for (Block *b = h->blocks; b;) { Block *n = b->next; free(b); b = n; }
    free(h);
}
static int overlaps(const GuestLocalHeap *h, uint32_t a, uint32_t size) {
    for (const Block *b = h->blocks; b; b = b->next)
        if (a < b->address + b->size && b->address < a + size) return 1;
    return 0;
}
int lh_alloc(GuestLocalHeap *h, uint32_t n, uint32_t *address) {
    if (!n || n > 0x1fff0000) return GM_RANGE;
    uint32_t size = (n + 4095) & ~4095u;
    if ((uint64_t)h->next_address + size > 0x2fff0000) return GM_RANGE;
    uint32_t a = h->next_address;
    while (overlaps(h, a, size)) { a += 4096; if ((uint64_t)a + size > 0x2fff0000) return GM_OVERLAP; }
    Block *b = calloc(1, sizeof(*b)); if (!b) return GM_RANGE;
    static const uint8_t zero[4096];
    for (uint32_t off = 0; off < size; off += 4096) {
        int rc = gm_write(h->memory, a + off, zero, 4096);
        if (rc) { free(b); return rc; }
    }
    b->address = a; b->size = size; b->next = h->blocks; h->blocks = b;
    h->next_address = a + size; *address = a; return GM_OK;
}
int lh_free(GuestLocalHeap *h, uint32_t a) {
    if (!a) return GM_OK;
    Block **slot = &h->blocks; while (*slot && (*slot)->address != a) slot = &(*slot)->next;
    if (!*slot) return GM_RANGE;
    Block *b = *slot; *slot = b->next; free(b);
    if (!h->blocks) h->next_address = 0x10000000;
    return GM_OK;
}
uint32_t lh_regions(GuestLocalHeap *h, uint32_t *pairs, uint32_t capacity) {
    uint32_t count = 0;
    for (Block *b = h->blocks; b; b = b->next) {
        if (count < capacity) { pairs[count * 2] = b->address; pairs[count * 2 + 1] = b->size; }
        count++;
    }
    return count;
}
