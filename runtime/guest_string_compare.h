#ifndef BURNOUT_GUEST_STRING_COMPARE_H
#define BURNOUT_GUEST_STRING_COMPARE_H
#include "guest_memory.h"
/* lstrcmpiW over the captured sort-NLS casing map.
 *
 * The comparison folds both operands through one casing table of the supplied
 * NLS image and compares the folded UTF-16 units; there is no collation,
 * locale tailoring, normalization or host locale involved. Only the BMP is
 * modelled: surrogate operands and unterminated operands stop explicitly
 * instead of being compared by a guessed rule. */
typedef struct GuestStringCompare GuestStringCompare;
int gls_create(const uint8_t *sort_nls,uint32_t size,GuestStringCompare **out);
void gls_destroy(GuestStringCompare *);
/* *result is the lstrcmpiW value (folded difference, else length difference). */
int gls_compare(const GuestStringCompare *,GuestMemory *,uint32_t left,uint32_t right,int32_t *result);
#endif
