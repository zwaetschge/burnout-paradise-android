#ifndef BURNOUT_GUEST_FLS_H
#define BURNOUT_GUEST_FLS_H
#include <stdint.h>
#define GFL_SLOTS 128u
#define GFL_CALLBACK_REQUIRED 0x10001u
/* One default fiber. Slot zero is reserved for the runtime. Guest callbacks
 * are stored as guest addresses; they are never cast to host function pointers.
 * No fiber switching, thread-exit callbacks or concurrent access is claimed. */
typedef struct GuestFls GuestFls;
GuestFls *gfl_create(void);
void gfl_destroy(GuestFls *);
uint32_t gfl_alloc(GuestFls *,uint32_t callback,uint32_t *index);
uint32_t gfl_get(GuestFls *,uint32_t index,uint32_t *value);
uint32_t gfl_set(GuestFls *,uint32_t index,uint32_t value);
/* Required non-null callback/data pairs stop without modifying the slot. */
uint32_t gfl_free(GuestFls *,uint32_t index);
void gfl_snapshot(const GuestFls *,uint32_t words[GFL_SLOTS*3]);
#endif
