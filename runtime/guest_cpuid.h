#ifndef BURNOUT_GUEST_CPUID_H
#define BURNOUT_GUEST_CPUID_H
#include <stdint.h>
#include <stddef.h>
typedef struct {uint32_t leaf,subleaf,subleaf_mask,values[4];} GuestCpuLeaf;
/* Explicit guest CPU model, never the host's x86 CPUID or Android hardware.
 * Missing leaves fail; advertised ISA still needs separately validated lifts.
 * The current captured Haswell model is diagnostic, not a release CPU profile. */
int gcp_query(const GuestCpuLeaf *,size_t,uint32_t leaf,uint32_t subleaf,uint32_t out[4]);
#endif
