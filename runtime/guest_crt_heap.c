/* UCRT heap bridges (api-ms-win-crt-heap-l1-1-0.dll).
 *
 * malloc/free/calloc/realloc/_recalloc/_aligned_malloc/_aligned_free/_msize
 * are projections onto the port-owned process heap
 * (runtime/guest_process_heap.c). The guest never sees a host pointer: the
 * returned address is a guest address inside GPH_BASE..GPH_LIMIT.
 *
 * Aligned blocks carry an eight-byte guest header directly in front of the
 * aligned address: [0] magic 0x414c4e44 ("ALND"), [4] the address returned by
 * the underlying heap allocation. _aligned_free validates the magic and stops
 * on a foreign pointer instead of freeing something it does not own.
 *
 * Documented limits: no low-fragmentation heap, no HeapSetInformation, no
 * debug fill patterns, no exception on out-of-memory (malloc returns NULL and
 * sets errno=ENOMEM, like UCRT). */
#include "guest_crt.h"
#include <string.h>

#define GCRT_ALIGNED_MAGIC 0x414c4e44u
#define GCRT_ALIGNED_OVERHEAD 16u

static int result_null(GcrtReturn *ret){
    ret->eax=0;return GM_OK;
}

static int crt_malloc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t size;
    int rc=gcrt_arg32(crt,sp,0,&size);
    if(rc)return rc;
    uint32_t address=0;
    rc=gcrt_heap_alloc(crt,0,size,&address);
    if(rc)return rc;
    if(!address && size)gcrt_set_errno(crt,GCRT_ENOMEM);
    ret->eax=address;return GM_OK;
}
static int crt_free(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    rc=gcrt_heap_free(crt,address);
    if(rc==GM_UNSUPPORTED)return gcrt_stop(crt,"free: pointer was not issued by malloc");
    if(rc)return rc;
    return result_null(ret);
}
static int crt_calloc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t count,size;
    int rc=gcrt_arg32(crt,sp,0,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&size);
    if(rc)return rc;
    uint64_t total=(uint64_t)count*size;
    if(total>UINT32_MAX){
        gcrt_set_errno(crt,GCRT_ENOMEM);
        return result_null(ret);
    }
    uint32_t address=0;
    rc=gcrt_heap_alloc(crt,8,(uint32_t)total,&address);
    if(rc)return rc;
    if(!address && total)gcrt_set_errno(crt,GCRT_ENOMEM);
    ret->eax=address;return GM_OK;
}
static int crt_realloc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,size;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&size);
    if(rc)return rc;
    if(!address){   /* realloc(NULL,n) == malloc(n) */
        uint32_t block=0;
        rc=gcrt_heap_alloc(crt,0,size,&block);
        if(rc)return rc;
        if(!block && size)gcrt_set_errno(crt,GCRT_ENOMEM);
        ret->eax=block;return GM_OK;
    }
    if(!size){      /* realloc(p,0) frees and returns NULL (UCRT behaviour) */
        rc=gcrt_heap_free(crt,address);
        if(rc)return rc;
        return result_null(ret);
    }
    uint32_t result=0;
    rc=gcrt_heap_realloc(crt,0,address,size,&result);
    if(rc==GM_UNSUPPORTED)
        return gcrt_stop(crt,"realloc: pointer was not issued by malloc");
    if(rc)return rc;
    if(!result)gcrt_set_errno(crt,GCRT_ENOMEM);
    ret->eax=result;return GM_OK;
}
static int crt_recalloc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,count,size;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&count);
    if(!rc)rc=gcrt_arg32(crt,sp,2,&size);
    if(rc)return rc;
    uint64_t total=(uint64_t)count*size;
    if(total>UINT32_MAX){
        gcrt_set_errno(crt,GCRT_ENOMEM);
        return result_null(ret);
    }
    if(!address){   /* _recalloc(NULL,n,size) == calloc(n,size) */
        uint32_t block=0;
        rc=gcrt_heap_alloc(crt,8,(uint32_t)total,&block);
        if(rc)return rc;
        if(!block && total)gcrt_set_errno(crt,GCRT_ENOMEM);
        ret->eax=block;return GM_OK;
    }
    if(!total){
        rc=gcrt_heap_free(crt,address);
        if(rc)return rc;
        return result_null(ret);
    }
    uint32_t result=0;
    rc=gcrt_heap_realloc(crt,8,address,(uint32_t)total,&result);
    if(rc)return rc;
    if(!result)gcrt_set_errno(crt,GCRT_ENOMEM);
    ret->eax=result;return GM_OK;
}
static int crt_aligned_malloc(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t size,alignment;
    int rc=gcrt_arg32(crt,sp,0,&size);
    if(!rc)rc=gcrt_arg32(crt,sp,1,&alignment);
    if(rc)return rc;
    if(!alignment || (alignment&(alignment-1))){
        gcrt_set_errno(crt,GCRT_EINVAL);
        return result_null(ret);
    }
    if(alignment<8)alignment=8;
    uint64_t total=(uint64_t)size+GCRT_ALIGNED_OVERHEAD+alignment;
    if(total>UINT32_MAX){
        gcrt_set_errno(crt,GCRT_ENOMEM);
        return result_null(ret);
    }
    uint32_t block=0;
    rc=gcrt_heap_alloc(crt,0,(uint32_t)total,&block);
    if(rc)return rc;
    if(!block){
        gcrt_set_errno(crt,GCRT_ENOMEM);
        return result_null(ret);
    }
    uint32_t aligned=(block+GCRT_ALIGNED_OVERHEAD+alignment-1)&~(alignment-1);
    uint8_t header[GCRT_ALIGNED_OVERHEAD];
    memset(header,0,sizeof(header));
    header[0]=(uint8_t)GCRT_ALIGNED_MAGIC;header[1]=(uint8_t)(GCRT_ALIGNED_MAGIC>>8);
    header[2]=(uint8_t)(GCRT_ALIGNED_MAGIC>>16);header[3]=(uint8_t)(GCRT_ALIGNED_MAGIC>>24);
    header[4]=(uint8_t)block;header[5]=(uint8_t)(block>>8);
    header[6]=(uint8_t)(block>>16);header[7]=(uint8_t)(block>>24);
    header[8]=(uint8_t)size;header[9]=(uint8_t)(size>>8);
    header[10]=(uint8_t)(size>>16);header[11]=(uint8_t)(size>>24);
    rc=gm_write(gcrt_memory(crt),aligned-GCRT_ALIGNED_OVERHEAD,header,
                GCRT_ALIGNED_OVERHEAD);
    if(rc)return rc;
    ret->eax=aligned;return GM_OK;
}
static int aligned_original(GuestCrt *crt,uint32_t address,uint32_t *original){
    if(!crt || !original)return GM_RANGE;
    if(!address)return GM_RANGE;
    uint8_t header[GCRT_ALIGNED_OVERHEAD];
    int rc=gm_read(gcrt_memory(crt),address-GCRT_ALIGNED_OVERHEAD,header,
                   GCRT_ALIGNED_OVERHEAD);
    if(rc)return gcrt_stop(crt,"_aligned_free: pointer was not issued by _aligned_malloc");
    uint32_t magic=(uint32_t)header[0]|((uint32_t)header[1]<<8)|
                   ((uint32_t)header[2]<<16)|((uint32_t)header[3]<<24);
    if(magic!=GCRT_ALIGNED_MAGIC)
        return gcrt_stop(crt,"_aligned_free: pointer was not issued by _aligned_malloc");
    *original=(uint32_t)header[4]|((uint32_t)header[5]<<8)|
              ((uint32_t)header[6]<<16)|((uint32_t)header[7]<<24);
    return GM_OK;
}
static int crt_aligned_free(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    if(!address)return result_null(ret);
    uint32_t original=0;
    rc=aligned_original(crt,address,&original);
    if(rc)return rc;
    rc=gcrt_heap_free(crt,original);
    if(rc)return rc;
    return result_null(ret);
}
static int crt_msize(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address,size=0;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    if(!address)return gcrt_stop(crt,"_msize(NULL)");
    rc=gcrt_heap_size(crt,address,&size);
    if(rc==GM_UNSUPPORTED)
        return gcrt_stop(crt,"_msize: pointer was not issued by malloc");
    if(rc)return rc;
    ret->eax=size;return GM_OK;
}
static int crt_aligned_msize(GuestCrt *crt,uint32_t sp,GcrtReturn *ret){
    uint32_t address;
    int rc=gcrt_arg32(crt,sp,0,&address);
    if(rc)return rc;
    uint8_t header[GCRT_ALIGNED_OVERHEAD];
    rc=gm_read(gcrt_memory(crt),address-GCRT_ALIGNED_OVERHEAD,header,
               GCRT_ALIGNED_OVERHEAD);
    if(rc)return rc;
    uint32_t magic=(uint32_t)header[0]|((uint32_t)header[1]<<8)|
                   ((uint32_t)header[2]<<16)|((uint32_t)header[3]<<24);
    if(magic!=GCRT_ALIGNED_MAGIC)
        return gcrt_stop(crt,"_aligned_msize: pointer was not issued by _aligned_malloc");
    ret->eax=(uint32_t)header[8]|((uint32_t)header[9]<<8)|
             ((uint32_t)header[10]<<16)|((uint32_t)header[11]<<24);
    return GM_OK;
}

#define HEAP_DLL "api-ms-win-crt-heap-l1-1-0.dll"
const GcrtExport crt_heap_exports[]={
    {HEAP_DLL,"malloc",0,crt_malloc},
    {HEAP_DLL,"free",0,crt_free},
    {HEAP_DLL,"calloc",0,crt_calloc},
    {HEAP_DLL,"realloc",0,crt_realloc},
    {HEAP_DLL,"_recalloc",0,crt_recalloc},
    {HEAP_DLL,"_aligned_malloc",0,crt_aligned_malloc},
    {HEAP_DLL,"_aligned_free",0,crt_aligned_free},
    {HEAP_DLL,"_msize",0,crt_msize},
    {HEAP_DLL,"_aligned_msize",0,crt_aligned_msize}
};
const uint32_t crt_heap_export_count=sizeof(crt_heap_exports)/sizeof(crt_heap_exports[0]);
