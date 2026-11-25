#ifndef BURNOUT_GUEST_ENVIRONMENT_H
#define BURNOUT_GUEST_ENVIRONMENT_H
#include "guest_memory.h"
typedef struct {const char *name,*value;} GuestEnvironmentEntry;
#define GE_BLOCK_BASE 0x74001000u
#define GE_BLOCK_SIZE 4096u
/* Process-owned, sorted UTF16 environment block, double-NUL terminated.
 * Stage into owned backing before mapping readonly guest memory. */
int ge_serialize_block(const GuestEnvironmentEntry *,uint32_t count,uint8_t *output,uint32_t capacity,uint32_t *written);
/* Process-owned ASCII profile. Host getenv() is intentionally not the source. */
int ge_get_a(GuestMemory *memory,const GuestEnvironmentEntry *entries,uint32_t count,
             uint32_t name,uint32_t buffer,uint32_t capacity,uint32_t last_error,uint32_t *result);
#endif
