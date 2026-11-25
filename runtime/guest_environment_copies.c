#include "guest_environment_copies.h"
#include <stdlib.h>
typedef struct Copy {uint32_t address;struct Copy *next;} Copy;
struct GuestEnvironmentCopies {GuestMemory *memory;GuestProcessHeap *heap;uint32_t source;Copy *copies;};
GuestEnvironmentCopies *gec_create(GuestMemory *memory,GuestProcessHeap *heap,uint32_t source){
    if(!memory || !heap || !source)return NULL;
    GuestEnvironmentCopies *copies=calloc(1,sizeof(*copies));
    if(copies){copies->memory=memory;copies->heap=heap;copies->source=source;}return copies;
}
void gec_destroy(GuestEnvironmentCopies *environment){
    if(!environment)return;
    for(Copy *p=environment->copies;p;){Copy *next=p->next;free(p);p=next;}free(environment);
}
int gec_get_w(GuestEnvironmentCopies *environment,uint32_t *address){
    if(!environment || !address)return GM_RANGE;
    uint8_t bytes[4096];int rc=gm_read(environment->memory,environment->source,bytes,sizeof(bytes));if(rc)return rc;
    uint32_t length=0;int previous_zero=0;
    for(uint32_t i=0;i<sizeof(bytes);i+=2){
        int zero=bytes[i]==0 && bytes[i+1]==0;
        if(zero && previous_zero){length=i+2;break;}previous_zero=zero;
    }
    if(!length)return GM_UNSUPPORTED;
    Copy *copy=malloc(sizeof(*copy));if(!copy)return GM_ALLOCATION;
    uint32_t block;rc=gph_alloc(environment->heap,GPH_HANDLE,8,length,&block);
    if(rc || !block){free(copy);return rc ? rc : GM_ALLOCATION;}
    rc=gm_write(environment->memory,block,bytes,length);
    if(rc){gph_free(environment->heap,GPH_HANDLE,0,block);free(copy);return rc;}
    copy->address=block;copy->next=environment->copies;environment->copies=copy;*address=block;return GM_OK;
}
int gec_free_w(GuestEnvironmentCopies *environment,uint32_t address){
    if(!environment)return GM_RANGE;
    Copy **slot=&environment->copies;while(*slot && (*slot)->address!=address)slot=&(*slot)->next;
    if(!*slot)return GM_UNSUPPORTED;
    int rc=gph_free(environment->heap,GPH_HANDLE,0,address);if(rc)return rc;
    Copy *copy=*slot;*slot=copy->next;free(copy);return GM_OK;
}
uint32_t gec_blocks(const GuestEnvironmentCopies *environment,uint32_t *addresses,uint32_t capacity){
    if(!environment)return 0;
    uint32_t count=0;for(Copy *p=environment->copies;p;p=p->next){if(addresses && count<capacity)addresses[count]=p->address;count++;}return count;
}
