#include "guest_tls.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
struct GuestTlsProcess {
    atomic_flag lock;
    uint64_t allocated[GT_SLOTS/64];
    GuestTlsThread *threads;
};
struct GuestTlsThread {
    GuestTlsProcess *process;
    GuestTlsThread *next;
    uint32_t error,values[GT_SLOTS];
};
static void lock(GuestTlsProcess *p) { while(atomic_flag_test_and_set_explicit(&p->lock,memory_order_acquire)){} }
static void unlock(GuestTlsProcess *p) { atomic_flag_clear_explicit(&p->lock,memory_order_release); }
static int allocated(GuestTlsProcess *p,uint32_t i) {return i<GT_SLOTS && (p->allocated[i/64]>>(i%64)&1);}
GuestTlsProcess *gt_process_create(void) {
    GuestTlsProcess *p=calloc(1,sizeof(*p));if(p)atomic_flag_clear(&p->lock);return p;
}
/* Caller has joined all host threads before destroying their process. */
void gt_process_destroy(GuestTlsProcess *p) {
    if(!p)return;
    GuestTlsThread *t=p->threads;
    while(t){GuestTlsThread *next=t->next;free(t);t=next;}free(p);
}
GuestTlsThread *gt_thread_create(GuestTlsProcess *p) {
    GuestTlsThread *t=calloc(1,sizeof(*t));if(!t)return NULL;t->process=p;
    lock(p);t->next=p->threads;p->threads=t;unlock(p);return t;
}
void gt_thread_destroy(GuestTlsThread *t) {
    GuestTlsProcess *p=t->process;lock(p);
    GuestTlsThread **slot=&p->threads;while(*slot && *slot!=t)slot=&(*slot)->next;
    if(*slot)*slot=t->next;unlock(p);free(t);
}
uint32_t gt_alloc(GuestTlsThread *t) {
    GuestTlsProcess *p=t->process;lock(p);
    for(uint32_t i=0;i<GT_SLOTS;i++)if(!allocated(p,i)) {
        p->allocated[i/64]|=UINT64_C(1)<<(i%64);
        for(GuestTlsThread *thread=p->threads;thread;thread=thread->next)thread->values[i]=0;
        unlock(p);return i;
    }
    t->error=8;unlock(p);return UINT32_MAX;
}
uint32_t gt_free(GuestTlsThread *t,uint32_t i) {
    GuestTlsProcess *p=t->process;lock(p);
    if(!allocated(p,i)){t->error=87;unlock(p);return 0;}
    p->allocated[i/64]&=~(UINT64_C(1)<<(i%64));unlock(p);return 1;
}
uint32_t gt_get(GuestTlsThread *t,uint32_t i) {
    GuestTlsProcess *p=t->process;lock(p);
    if(i>=GT_SLOTS || (i>=GT_MINIMUM && !allocated(p,i))){t->error=87;unlock(p);return 0;}
    t->error=0;uint32_t value=t->values[i];unlock(p);return value;
}
uint32_t gt_set(GuestTlsThread *t,uint32_t i,uint32_t value) {
    GuestTlsProcess *p=t->process;lock(p);
    if(i>=GT_SLOTS || (i>=GT_MINIMUM && !allocated(p,i))){t->error=87;unlock(p);return 0;}
    t->values[i]=value;unlock(p);return 1;
}
uint32_t gt_error(GuestTlsThread *t){return t->error;}
void gt_set_error(GuestTlsThread *t,uint32_t value){t->error=value;}
void gt_snapshot(GuestTlsThread *t,uint32_t values[GT_SLOTS]) {
    lock(t->process);memcpy(values,t->values,sizeof(t->values));unlock(t->process);
}
