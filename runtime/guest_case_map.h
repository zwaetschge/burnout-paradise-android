#ifndef BURNOUT_GUEST_CASE_MAP_H
#define BURNOUT_GUEST_CASE_MAP_H
#include "guest_memory.h"
typedef struct GuestCaseMaps GuestCaseMaps;
int gcm_create(const uint8_t *sort_nls,uint32_t size,GuestCaseMaps **out);
void gcm_destroy(GuestCaseMaps *);
/* File-system casing only: LOWERCASE/UPPERCASE, BMP excluding surrogates.
 * No linguistic casing, sort keys, alternate locales or supplementary pairs. */
int gcm_string(const GuestCaseMaps *,GuestMemory *,uint32_t flags,uint32_t input,int32_t count,uint32_t output,int32_t capacity,uint32_t *result,uint32_t *error);
#endif
