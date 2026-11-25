#ifndef BURNOUT_GUEST_LOCALE_H
#define BURNOUT_GUEST_LOCALE_H
#include "guest_memory.h"
typedef struct {uint32_t ansi_code_page,installed;uint16_t decode[256];uint8_t info[18],encode[65536];} GuestLocale;
/* Only the selected supplied compatibility profile; converters are separate. */
int gl_initialize(GuestLocale *,uint32_t ansi_code_page);
int gl_ansi_code_page(const GuestLocale *,uint32_t *value);
int gl_install_cp1252(GuestLocale *,const uint8_t *nls,uint32_t size);
int gl_valid_code_page(const GuestLocale *,uint32_t code_page,uint32_t *value);
int gl_decode_byte(const GuestLocale *,uint32_t byte,uint32_t *unicode);
int gl_cp_info(const GuestLocale *,GuestMemory *,uint32_t code_page,uint32_t output,uint32_t *success,uint32_t *error);
int gl_mb_to_wide(const GuestLocale *,GuestMemory *,uint32_t code_page,uint32_t flags,uint32_t input,int32_t bytes,uint32_t output,int32_t capacity,uint32_t *result,uint32_t *error);
int gl_wide_to_mb(const GuestLocale *,GuestMemory *,uint32_t code_page,uint32_t flags,uint32_t input,int32_t count,uint32_t output,int32_t capacity,int default_char,uint32_t *used_default,uint32_t *result,uint32_t *error);
#endif
