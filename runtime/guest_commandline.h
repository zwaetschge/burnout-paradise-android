#ifndef BURNOUT_COMMANDLINE_H
#define BURNOUT_COMMANDLINE_H
#include "local_heap.h"
/* UTF-16 input, 32-bit pointer vector, one LocalFree-owned block.
 * Documented basic quoting/backslashes are supported; ambiguous adjacent
 * quotes inside quoted text stop with GM_UNSUPPORTED pending parity tests. */
int gc_argv(GuestMemory *memory,GuestLocalHeap *heap,uint32_t command,
            uint32_t count_out,uint32_t fallback_filename,uint32_t *argv);
#endif
