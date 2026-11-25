#ifndef BURNOUT_GUEST_SHARED_DATA_H
#define BURNOUT_GUEST_SHARED_DATA_H
#include <stdint.h>
#include "guest_memory.h"
#define GS_BASE 0x7ffe0000u
#define GS_SIZE 4096u
#define GS_MASK_SIZE 512u
/* Only reference-profile version fields are implemented, not blank/zero time,
 * processor, debugger, licensing or system-call state. Protect with the bitmap. */
int gs_version_fields(uint8_t *page,uint8_t *bitmap,uint32_t major,uint32_t minor,uint32_t build);
/* Diagnostic Haswell profile only: PF0..11 from the guest CPU and FXSAVE model.
 * This does not assert that every advertised ISA has already been lifted. */
int gs_haswell_legacy_features(uint8_t *page,uint8_t *bitmap,const uint32_t leaf1[4]);
/* PF12..15: checked guest DEP, SSE3/CX16, absent Itanium CMP8XCHG16. */
int gs_haswell_nx_features(uint8_t *page,uint8_t *bitmap,const uint32_t leaf1[4],const uint32_t extended1[4]);
/* PF16..19: no processor channels enabled in this runtime, diagnostic XSAVE,
 * no ARM instruction set in the x86 guest. Not host hardware discovery. */
int gs_haswell_xstate_features(uint8_t *page,uint8_t *bitmap,const uint32_t leaf1[4]);
/* NumberOfPhysicalPages is measured in 4-KB guest pages, not host pages. */
int gs_physical_memory_field(uint8_t *page,uint8_t *bitmap,uint64_t physical_bytes);
int gs_host_physical_bytes(uint64_t *physical_bytes);
/* Process-owned ASCII SystemRoot serialized as the complete WCHAR[260] field. */
int gs_system_root_field(uint8_t *page,uint8_t *bitmap,const char *root);
int gs_system_time_field(uint8_t *page,uint8_t *bitmap,uint64_t filetime);
int gs_timezone_field(uint8_t *page,uint8_t *bitmap,int64_t bias);
int gs_interrupt_time_field(uint8_t *page,uint8_t *bitmap,uint64_t ticks);
/* Read the declared guest feature array; unimplemented in-range bytes fail. */
int gs_processor_feature(GuestMemory *,uint32_t feature,uint32_t *present);
#endif
