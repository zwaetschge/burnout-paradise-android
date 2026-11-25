#ifndef BURNOUT_GUEST_APISET_H
#define BURNOUT_GUEST_APISET_H
#include <stddef.h>
typedef struct {const char *prefix,*host;} GuestApiSet;
/* Explicit-load default targets from a verified schema, not a wildcard. */
int gas_resolve(const GuestApiSet *,size_t count,const char *name,const char **host);
#endif
