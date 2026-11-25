#ifndef BURNOUT_GUEST_CHARACTER_TYPE_H
#define BURNOUT_GUEST_CHARACTER_TYPE_H
#include "guest_memory.h"
typedef struct GuestCharacterTypes GuestCharacterTypes;
int gct_create(const uint8_t *sort_nls,uint32_t size,GuestCharacterTypes **out);
void gct_destroy(GuestCharacterTypes *);
int gct_string_type(const GuestCharacterTypes *,GuestMemory *,uint32_t type,uint32_t input,int32_t count,uint32_t output,uint32_t *success,uint32_t *error);
#endif
