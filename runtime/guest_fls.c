#include "guest_fls.h"
#include <pthread.h>
#include <stdlib.h>
/* Fiber-local storage for threads that never convert to fibers: every thread
 * is its own default fiber, so slot *allocation* is process-wide and slot
 * *values* are per thread (FlsSetValue in one thread is invisible in another).
 * The CRT keeps its per-thread data block in an FLS slot; one shared value per
 * slot made a new thread pick up another thread's start routine.
 *
 * Values live in thread-local storage and are tagged with the generation of
 * the slot they were stored for, so a slot that is freed and allocated again
 * reads as zero in every thread. Callbacks at thread exit are not modelled. */
#define GFL_INSTANCES 4u
struct GuestFls {
    uint32_t used[GFL_SLOTS],callback[GFL_SLOTS],generation[GFL_SLOTS];
    uint32_t id;
    pthread_mutex_t lock;
};
static __thread struct {uint32_t generation[GFL_SLOTS],value[GFL_SLOTS];} thread_values[GFL_INSTANCES];
static pthread_mutex_t global_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_id,next_generation;
static uint32_t new_generation(void){
    pthread_mutex_lock(&global_lock);uint32_t g=++next_generation;pthread_mutex_unlock(&global_lock);return g;
}
GuestFls *gfl_create(void){
    GuestFls *p=calloc(1,sizeof(*p));
    if(!p)return NULL;
    p->used[0]=1;pthread_mutex_init(&p->lock,NULL);
    pthread_mutex_lock(&global_lock);p->id=next_id++%GFL_INSTANCES;pthread_mutex_unlock(&global_lock);
    return p;
}
void gfl_destroy(GuestFls *p){if(p){pthread_mutex_destroy(&p->lock);free(p);}}
static int valid(const GuestFls *p,uint32_t index){return p && index>0 && index<GFL_SLOTS && p->used[index];}
static uint32_t current(const GuestFls *p,uint32_t index){
    return thread_values[p->id].generation[index]==p->generation[index]?thread_values[p->id].value[index]:0;
}
uint32_t gfl_alloc(GuestFls *p,uint32_t callback,uint32_t *index){
    if(!p || !index)return 87;
    pthread_mutex_lock(&p->lock);
    for(uint32_t i=1;i<GFL_SLOTS;i++)if(!p->used[i]){
        p->used[i]=1;p->callback[i]=callback;p->generation[i]=new_generation();
        pthread_mutex_unlock(&p->lock);
        *index=i;return 0;
    }
    pthread_mutex_unlock(&p->lock);
    return 8;
}
uint32_t gfl_get(GuestFls *p,uint32_t index,uint32_t *value){
    if(!value || !valid(p,index))return 87;
    *value=current(p,index);return 0;
}
uint32_t gfl_set(GuestFls *p,uint32_t index,uint32_t value){
    if(!valid(p,index))return 87;
    thread_values[p->id].generation[index]=p->generation[index];
    thread_values[p->id].value[index]=value;return 0;
}
uint32_t gfl_free(GuestFls *p,uint32_t index){
    if(!valid(p,index))return 87;
    if(p->callback[index] && current(p,index))return GFL_CALLBACK_REQUIRED;
    pthread_mutex_lock(&p->lock);
    p->used[index]=p->callback[index]=0;p->generation[index]=0;
    pthread_mutex_unlock(&p->lock);
    return 0;
}
/* used / callback / the calling thread's value */
void gfl_snapshot(const GuestFls *p,uint32_t *out){
    for(uint32_t i=0;i<GFL_SLOTS;i++){
        out[3*i]=p->used[i];out[3*i+1]=p->callback[i];out[3*i+2]=p->used[i]?current(p,i):0;
    }
}
