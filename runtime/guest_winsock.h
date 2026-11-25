#ifndef GUEST_WINSOCK_H
#define GUEST_WINSOCK_H
#include "guest_memory.h"
typedef struct {uint32_t references;} GuestWinsock;
/* Initialization and the matching cleanup only. Unknown socket APIs remain
 * explicit dispatch stops. */
int gws_backend_ready(void);
int gws_startup_value(GuestMemory *,GuestWinsock *,uint32_t version,uint32_t output,uint32_t ready,uint32_t *result);
int gws_startup(GuestMemory *,GuestWinsock *,uint32_t version,uint32_t output,uint32_t *result);
/* WSACleanup: only a matching initialization count is modelled; a cleanup
 * without an outstanding WSAStartup stops instead of inventing success. */
int gws_cleanup(GuestWinsock *,uint32_t *result);
#endif
