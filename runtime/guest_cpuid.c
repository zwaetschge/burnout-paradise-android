#include "guest_cpuid.h"
#include "guest_memory.h"
#include <stdatomic.h>
#include <string.h>
int gcp_query(const GuestCpuLeaf *model,size_t count,uint32_t leaf,uint32_t subleaf,uint32_t out[4]){
    if(!model || !out)return GM_RANGE;
    for(size_t i=0;i<count;i++)if(model[i].leaf==leaf && (subleaf&model[i].subleaf_mask)==model[i].subleaf){
        atomic_thread_fence(memory_order_seq_cst);
        memcpy(out,model[i].values,4*sizeof(*out));
        atomic_thread_fence(memory_order_seq_cst);return 0;
    }
    return GM_UNSUPPORTED;
}
