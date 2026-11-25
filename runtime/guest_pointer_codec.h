#ifndef GUEST_POINTER_CODEC_H
#define GUEST_POINTER_CODEC_H
#include <stdint.h>
/* PE32 pointer values are transformed as integers; never dereferenced. */
int gpc_transform(uint32_t cookie,uint32_t which,uint32_t pointer,uint32_t *result);
int gpc_random_cookie(uint32_t *cookie);
#endif
