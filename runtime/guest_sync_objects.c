#define _POSIX_C_SOURCE 200809L
#include "guest_sync_objects.h"
#include "guest_handles.h"
#include "guest_critical.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* One host-side object per guest kernel object. All state is guarded by a
 * single module mutex; per-object condition variables carry the blocking
 * waits and gso_any wakes WaitForMultipleObjects. A waiter holds a reference
 * so a concurrent CloseHandle cannot free the object under it. */
typedef struct GsoObject {
    uint32_t kind,handle,manual_reset,signaled,owner,abandoned;
    int32_t count,maximum;
    uint32_t refs,dead;
    pthread_cond_t cond;
    struct GsoObject *next;
} GsoObject;

static GsoObject *objects;
static pthread_mutex_t gso_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gso_any;
static pthread_condattr_t gso_attr;
static pthread_once_t gso_once=PTHREAD_ONCE_INIT;
static int (*thread_wait_hook)(uint32_t,uint32_t,uint32_t *);

/* Every wait deadline is computed from CLOCK_MONOTONIC, so the condition
 * variables must use the same clock (the default is CLOCK_REALTIME). */
static void init_cond_clock(void){
    pthread_condattr_init(&gso_attr);
    pthread_condattr_setclock(&gso_attr,CLOCK_MONOTONIC);
    pthread_cond_init(&gso_any,&gso_attr);
}
static void ensure_cond_clock(void){pthread_once(&gso_once,init_cond_clock);}

static void put32(uint8_t *b,uint32_t v){
    b[0]=(uint8_t)v;b[1]=(uint8_t)(v>>8);b[2]=(uint8_t)(v>>16);b[3]=(uint8_t)(v>>24);
}
static uint32_t get32(const uint8_t *b){
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int last_error(GuestMemory *memory,uint32_t teb,uint32_t value){
    uint8_t bytes[4];put32(bytes,value);return gm_write(memory,teb+0x34,bytes,4);
}
static int current_thread(GuestMemory *memory,uint32_t teb,uint32_t *thread){
    if(!memory || !thread)return GM_UNSUPPORTED;
    uint8_t bytes[4];int rc=gm_read(memory,teb+0x24,bytes,4);if(rc)return rc;
    *thread=get32(bytes);
    /* Without a serialized guest thread identity ownership cannot be modelled. */
    return *thread ? GM_OK : GM_UNSUPPORTED;
}
static int deadline_after(uint32_t milliseconds,struct timespec *ts){
    struct timespec now;
    if(clock_gettime(CLOCK_MONOTONIC,&now))return GM_UNSUPPORTED;
    uint64_t total=(uint64_t)now.tv_sec*1000u+(uint64_t)(now.tv_nsec/1000000)+milliseconds;
    ts->tv_sec=(time_t)(total/1000u);
    ts->tv_nsec=(long)((total%1000u)*1000000u);
    return GM_OK;
}
static GsoObject *object_new(uint32_t kind){
    ensure_cond_clock();
    GsoObject *o=calloc(1,sizeof(*o));
    if(!o)return NULL;
    o->kind=kind;o->refs=1;
    if(pthread_cond_init(&o->cond,&gso_attr)){free(o);return NULL;}
    return o;
}
static void object_free(GsoObject *o){
    pthread_cond_destroy(&o->cond);free(o);
}
static void object_destroy(void *p){
    GsoObject *o=p;int last;
    pthread_mutex_lock(&gso_lock);
    o->dead=1;
    GsoObject **slot=&objects;while(*slot && *slot!=o)slot=&(*slot)->next;
    if(*slot)*slot=o->next;
    last=(--o->refs==0);
    pthread_mutex_unlock(&gso_lock);
    if(last)object_free(o);
}
/* Caller holds gso_lock. */
static void drop_reference(GsoObject *o){
    if(--o->refs==0)object_free(o);
}
static void notify(GsoObject *o){
    ensure_cond_clock();
    pthread_cond_broadcast(&o->cond);pthread_cond_broadcast(&gso_any);
}
/* Returns a referenced object of one of the waitable kinds, or NULL. *kind_out
 * always receives the live kind so the caller can report a wrong-kind handle. */
static GsoObject *acquire(uint32_t handle,uint32_t *kind_out){
    uint32_t kind=GH_KIND_NONE;GsoObject *o=NULL;
    pthread_mutex_lock(&gso_lock);
    void *object=gh_lookup(handle,GH_KIND_NONE,&kind);
    if(object && (kind==GH_KIND_EVENT || kind==GH_KIND_MUTEX || kind==GH_KIND_SEMAPHORE)){
        o=object;o->refs++;
    }
    pthread_mutex_unlock(&gso_lock);
    if(kind_out)*kind_out=kind;
    return o;
}
static int publish(GuestMemory *memory,uint32_t teb,GsoObject *o,uint32_t *handle){
    uint32_t h=gh_insert(o->kind,o,object_destroy);
    if(!h){
        object_free(o);
        int rc=last_error(memory,teb,GSO_ERROR_NOT_ENOUGH_MEMORY);if(rc)return rc;
        *handle=0;return GM_OK;
    }
    o->handle=h;
    pthread_mutex_lock(&gso_lock);o->next=objects;objects=o;pthread_mutex_unlock(&gso_lock);
    *handle=h;return GM_OK;
}
/* 1 = taken (value set), 0 = not signalled yet, -1 = modelled stop. */
static int take(GsoObject *o,uint32_t thread,uint32_t *value){
    if(o->kind==GH_KIND_EVENT){
        if(!o->signaled)return 0;
        if(!o->manual_reset)o->signaled=0;
        *value=GSO_WAIT_OBJECT_0;return 1;
    }
    if(o->kind==GH_KIND_MUTEX){
        if(o->abandoned){
            o->abandoned=0;o->owner=thread;o->count=1;
            *value=GSO_WAIT_ABANDONED;return 1;
        }
        if(!o->owner){o->owner=thread;o->count=1;*value=GSO_WAIT_OBJECT_0;return 1;}
        if(o->owner==thread){
            if(o->count==INT32_MAX)return -1; /* recursion overflow is a stop */
            o->count++;*value=GSO_WAIT_OBJECT_0;return 1;
        }
        return 0;
    }
    if(o->kind==GH_KIND_SEMAPHORE){
        if(o->count<=0)return 0;
        o->count--;*value=GSO_WAIT_OBJECT_0;return 1;
    }
    return -1;
}
/* Caller holds a reference and gso_lock is taken inside. */
static int wait_one(GsoObject *o,uint32_t thread,uint32_t milliseconds,uint32_t *out){
    struct timespec limit;int rc=GM_OK;uint32_t value=GSO_WAIT_TIMEOUT;
    if(milliseconds!=GSO_INFINITE){rc=deadline_after(milliseconds,&limit);if(rc)return rc;}
    pthread_mutex_lock(&gso_lock);
    for(;;){
        if(o->dead){rc=GM_UNSUPPORTED;break;} /* closed while the wait was pending */
        int state=take(o,thread,&value);
        if(state==1)break;
        if(state<0){rc=GM_UNSUPPORTED;break;}
        int err;
        if(milliseconds==GSO_INFINITE)err=pthread_cond_wait(&o->cond,&gso_lock);
        else err=pthread_cond_timedwait(&o->cond,&gso_lock,&limit);
        if(err==ETIMEDOUT){value=GSO_WAIT_TIMEOUT;break;}
        if(err){rc=GM_UNSUPPORTED;break;}
    }
    pthread_mutex_unlock(&gso_lock);
    if(!rc)*out=value;
    return rc;
}
int gso_create_event(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t manual_reset,
                     uint32_t initial_state,uint32_t name,uint32_t *handle){
    if(!memory || !handle)return GM_UNSUPPORTED;
    /* SECURITY_ATTRIBUTES and named objects are not modelled. */
    if(attributes)return GM_UNSUPPORTED;   /* name allowed: single-process model, fresh local object */
    (void)name;
    if(manual_reset>1 || initial_state>1)return GM_UNSUPPORTED;
    GsoObject *o=object_new(GH_KIND_EVENT);if(!o)return GM_UNSUPPORTED;
    o->manual_reset=manual_reset;o->signaled=initial_state;
    return publish(memory,teb,o,handle);
}
int gso_create_event_w(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t manual_reset,
                       uint32_t initial_state,uint32_t name,uint32_t *handle){
    /* The wide entry point shares the model: a non-NULL name is a stop either
     * way, so no ANSI/UTF-16 name decoding is needed or claimed. */
    return gso_create_event(memory,teb,attributes,manual_reset,initial_state,name,handle);
}
int gso_set_event(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint32_t kind=GH_KIND_NONE;GsoObject *o=acquire(handle,&kind);
    if(!o){
        int rc=last_error(memory,teb,GSO_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=0;return GM_OK;
    }
    int rc=GM_OK;
    pthread_mutex_lock(&gso_lock);
    if(o->kind!=GH_KIND_EVENT)rc=GM_UNSUPPORTED;
    else{o->signaled=1;notify(o);}
    drop_reference(o);
    pthread_mutex_unlock(&gso_lock);
    if(!rc)*result=1;
    return rc;
}
int gso_reset_event(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint32_t kind=GH_KIND_NONE;GsoObject *o=acquire(handle,&kind);
    if(!o){
        int rc=last_error(memory,teb,GSO_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=0;return GM_OK;
    }
    int rc=GM_OK;
    pthread_mutex_lock(&gso_lock);
    if(o->kind!=GH_KIND_EVENT)rc=GM_UNSUPPORTED;
    else{o->signaled=0;notify(o);}
    drop_reference(o);
    pthread_mutex_unlock(&gso_lock);
    if(!rc)*result=1;
    return rc;
}
int gso_create_mutex(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t initial_owner,
                     uint32_t name,uint32_t *handle){
    if(!memory || !handle)return GM_UNSUPPORTED;
    if(attributes)return GM_UNSUPPORTED;   /* name allowed: single-process model, fresh local object */
    (void)name;
    if(initial_owner>1)return GM_UNSUPPORTED;
    uint32_t thread=0;
    if(initial_owner){int rc=current_thread(memory,teb,&thread);if(rc)return rc;}
    GsoObject *o=object_new(GH_KIND_MUTEX);if(!o)return GM_UNSUPPORTED;
    if(initial_owner){o->owner=thread;o->count=1;}
    return publish(memory,teb,o,handle);
}
int gso_create_mutex_w(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t initial_owner,
                       uint32_t name,uint32_t *handle){
    return gso_create_mutex(memory,teb,attributes,initial_owner,name,handle);
}
int gso_release_mutex(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint32_t thread;int rc=current_thread(memory,teb,&thread);if(rc)return rc;
    uint32_t kind=GH_KIND_NONE;GsoObject *o=acquire(handle,&kind);
    if(!o){
        rc=last_error(memory,teb,GSO_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=0;return GM_OK;
    }
    int not_owner=0;
    pthread_mutex_lock(&gso_lock);
    if(o->kind!=GH_KIND_MUTEX)rc=GM_UNSUPPORTED;
    else if(o->owner!=thread)not_owner=1; /* documented Win32 failure */
    else{
        if(--o->count<=0){o->count=0;o->owner=0;}
        notify(o);
    }
    drop_reference(o);
    pthread_mutex_unlock(&gso_lock);
    if(rc)return rc;
    if(not_owner){
        rc=last_error(memory,teb,GSO_ERROR_NOT_OWNER);if(rc)return rc;
        *result=0;return GM_OK;
    }
    *result=1;return GM_OK;
}
int gso_create_semaphore(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t initial,
                         uint32_t maximum,uint32_t name,uint32_t *handle){
    if(!memory || !handle)return GM_UNSUPPORTED;
    if(attributes)return GM_UNSUPPORTED;   /* name allowed: single-process model, fresh local object */
    (void)name;
    if(!maximum || initial>maximum || maximum>INT32_MAX)return GM_UNSUPPORTED;
    GsoObject *o=object_new(GH_KIND_SEMAPHORE);if(!o)return GM_UNSUPPORTED;
    o->count=(int32_t)initial;o->maximum=(int32_t)maximum;
    return publish(memory,teb,o,handle);
}
int gso_create_semaphore_w(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t initial,
                           uint32_t maximum,uint32_t name,uint32_t *handle){
    return gso_create_semaphore(memory,teb,attributes,initial,maximum,name,handle);
}
int gso_release_semaphore(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t release_count,
                          uint32_t previous_count,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint32_t kind=GH_KIND_NONE;GsoObject *o=acquire(handle,&kind);
    if(!o){
        int rc=last_error(memory,teb,GSO_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=0;return GM_OK;
    }
    int rc=GM_OK,failure=0;int32_t previous=0;
    pthread_mutex_lock(&gso_lock);
    if(o->kind!=GH_KIND_SEMAPHORE)rc=GM_UNSUPPORTED;
    else if(!release_count)failure=GSO_ERROR_INVALID_PARAMETER;
    else if((int64_t)o->count+(int64_t)release_count>(int64_t)o->maximum)
        failure=GSO_ERROR_TOO_MANY_POSTS;
    else{
        previous=o->count;o->count+=(int32_t)release_count;notify(o);
    }
    drop_reference(o);
    pthread_mutex_unlock(&gso_lock);
    if(rc)return rc;
    if(failure){
        rc=last_error(memory,teb,failure);if(rc)return rc;
        *result=0;return GM_OK;
    }
    if(previous_count){
        uint8_t bytes[4];put32(bytes,(uint32_t)previous);
        rc=gm_write(memory,previous_count,bytes,4);if(rc)return rc;
    }
    *result=1;return GM_OK;
}
int gso_wait_single(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t milliseconds,
                    uint32_t *result){
    return gso_wait_single_ex(memory,teb,handle,milliseconds,0,result);
}
int gso_wait_single_ex(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t milliseconds,
                       uint32_t alertable,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    /* Alertable: nothing in this runtime can queue an APC (see gta_sleep_ex),
     * so the queue is always empty and the wait is the plain wait. */
    (void)alertable;
    uint32_t thread;int rc=current_thread(memory,teb,&thread);if(rc)return rc;
    uint32_t kind=GH_KIND_NONE;GsoObject *o=acquire(handle,&kind);
    if(!o){
        if(kind==GH_KIND_THREAD){
            if(!thread_wait_hook)return GM_UNSUPPORTED;
            return thread_wait_hook(handle,milliseconds,result);
        }
        if(kind!=GH_KIND_NONE)return GM_UNSUPPORTED; /* non-waitable object kind */
        rc=last_error(memory,teb,GSO_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=GSO_WAIT_FAILED;return GM_OK;
    }
    uint32_t value=GSO_WAIT_TIMEOUT;
    rc=wait_one(o,thread,milliseconds,&value);
    pthread_mutex_lock(&gso_lock);drop_reference(o);pthread_mutex_unlock(&gso_lock);
    if(rc)return rc;
    *result=value;return GM_OK;
}
int gso_wait_multiple(GuestMemory *memory,uint32_t teb,uint32_t handles,uint32_t count,
                      uint32_t wait_all,uint32_t milliseconds,uint32_t *result){
    return gso_wait_multiple_ex(memory,teb,handles,count,wait_all,milliseconds,0,result);
}
int gso_wait_multiple_ex(GuestMemory *memory,uint32_t teb,uint32_t handles,uint32_t count,
                         uint32_t wait_all,uint32_t milliseconds,uint32_t alertable,
                         uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    (void)alertable;                /* empty APC queue, see gso_wait_single_ex */
    if(!count || count>GSO_MAXIMUM_WAIT_OBJECTS || wait_all>1)return GM_UNSUPPORTED;
    ensure_cond_clock();
    uint32_t thread;int rc=current_thread(memory,teb,&thread);if(rc)return rc;
    uint8_t raw[GSO_MAXIMUM_WAIT_OBJECTS*4];
    rc=gm_read(memory,handles,raw,count*4);if(rc)return rc;
    GsoObject *set[GSO_MAXIMUM_WAIT_OBJECTS];
    for(uint32_t i=0;i<count;i++){
        uint32_t kind=GH_KIND_NONE;
        set[i]=acquire(get32(raw+i*4),&kind);
        if(!set[i]){
            /* A thread handle in a multiple wait would need a second wake
             * source; report it instead of pretending to wait on it. */
            for(uint32_t k=0;k<i;k++){
                pthread_mutex_lock(&gso_lock);drop_reference(set[k]);pthread_mutex_unlock(&gso_lock);
            }
            if(kind==GH_KIND_NONE){
                rc=last_error(memory,teb,GSO_ERROR_INVALID_HANDLE);if(rc)return rc;
                *result=GSO_WAIT_FAILED;return GM_OK;
            }
            return GM_UNSUPPORTED;
        }
    }
    struct timespec limit;
    if(milliseconds!=GSO_INFINITE){rc=deadline_after(milliseconds,&limit);if(rc)goto done;}
    uint32_t value=GSO_WAIT_TIMEOUT;
    pthread_mutex_lock(&gso_lock);
    for(;;){
        int dead=0;
        for(uint32_t i=0;i<count;i++)if(set[i]->dead)dead=1;
        if(dead){rc=GM_UNSUPPORTED;break;}
        uint32_t picked=GSO_WAIT_FAILED;int abandoned=0;
        if(wait_all){
            int ready=1;
            for(uint32_t i=0;i<count;i++){
                GsoObject *o=set[i];int state;
                if(o->kind==GH_KIND_EVENT)state=o->signaled;
                else if(o->kind==GH_KIND_MUTEX)
                    state=o->abandoned || !o->owner || o->owner==thread;
                else state=o->count>0;
                if(!state){ready=0;break;}
            }
            if(ready){
                for(uint32_t i=0;i<count;i++){
                    uint32_t one=GSO_WAIT_OBJECT_0;
                    /* A mid-take model stop must not leave a half-taken set. */
                    if(take(set[i],thread,&one)<0){rc=GM_UNSUPPORTED;break;}
                    if(one==GSO_WAIT_ABANDONED)abandoned=1;
                }
                if(rc)break;
                picked=abandoned?GSO_WAIT_ABANDONED:GSO_WAIT_OBJECT_0;notify(set[0]);
            }
        }else{
            for(uint32_t i=0;i<count;i++){
                uint32_t one=GSO_WAIT_OBJECT_0;
                int state=take(set[i],thread,&one);
                if(state<0){rc=GM_UNSUPPORTED;break;}
                if(state==1){
                    picked=(one==GSO_WAIT_ABANDONED?GSO_WAIT_ABANDONED:GSO_WAIT_OBJECT_0)+i;
                    notify(set[i]);break;
                }
            }
            if(rc)break;
        }
        if(picked!=GSO_WAIT_FAILED){value=picked;break;}
        int err;
        if(milliseconds==GSO_INFINITE)err=pthread_cond_wait(&gso_any,&gso_lock);
        else err=pthread_cond_timedwait(&gso_any,&gso_lock,&limit);
        if(err==ETIMEDOUT){value=GSO_WAIT_TIMEOUT;break;}
        if(err){rc=GM_UNSUPPORTED;break;}
    }
    pthread_mutex_unlock(&gso_lock);
    if(!rc)*result=value;
done:
    for(uint32_t i=0;i<count;i++){
        pthread_mutex_lock(&gso_lock);drop_reference(set[i]);pthread_mutex_unlock(&gso_lock);
    }
    return rc;
}
int gso_initialize_critical_section(GuestMemory *memory,uint32_t section,uint32_t spin,
                                    uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    /* flags 0 = InitializeCriticalSection / ...AndSpinCount, one guest CPU. */
    int rc=gcs_initialize(memory,section,spin,0,1);if(rc)return rc;
    *result=1;return GM_OK;
}
int gso_delete_critical_section(GuestMemory *memory,uint32_t section){
    if(!memory)return GM_UNSUPPORTED;
    return gcs_delete(memory,section);
}
int gso_abandon_thread(GuestMemory *memory,uint32_t thread){
    (void)memory;
    if(!thread)return GM_UNSUPPORTED;
    pthread_mutex_lock(&gso_lock);
    for(GsoObject *o=objects;o;o=o->next)
        if(o->kind==GH_KIND_MUTEX && o->owner==thread){
            o->abandoned=1;o->owner=0;o->count=0;notify(o);
        }
    pthread_mutex_unlock(&gso_lock);
    return GM_OK;
}
void gso_set_thread_wait_hook(int (*fn)(uint32_t,uint32_t,uint32_t *)){
    pthread_mutex_lock(&gso_lock);thread_wait_hook=fn;pthread_mutex_unlock(&gso_lock);
}
uint32_t gso_object_count(uint32_t kind){
    uint32_t n=0;
    pthread_mutex_lock(&gso_lock);
    for(GsoObject *o=objects;o;o=o->next)if(!kind || o->kind==kind)n++;
    pthread_mutex_unlock(&gso_lock);
    return n;
}
void gso_reset_for_tests(void){
    /* Test-only teardown: close every object handle this module issued. A
     * duplicated handle keeps the object alive and ends the loop early, which
     * is reported by gso_object_count rather than silently freed. */
    for(uint32_t guard=0;objects && guard<GH_MAX_HANDLES;guard++){
        pthread_mutex_lock(&gso_lock);
        uint32_t handle=objects->handle;
        pthread_mutex_unlock(&gso_lock);
        if(!gh_close(handle))break;
    }
    ensure_cond_clock();
    pthread_mutex_lock(&gso_lock);pthread_cond_broadcast(&gso_any);pthread_mutex_unlock(&gso_lock);
}
/* Wiring list for runtime/win32_api_table.c. Argument bytes are the stdcall
 * stack bytes after the return address; the adapter supplies GuestMemory*,
 * the guest TEB and the *result out-parameter. */
const GsoExport gso_exports[]={
    {"kernel32.dll","CreateEventA",16,(void*)gso_create_event},
    {"kernel32.dll","CreateEventW",16,(void*)gso_create_event_w},
    {"kernel32.dll","SetEvent",4,(void*)gso_set_event},
    {"kernel32.dll","ResetEvent",4,(void*)gso_reset_event},
    {"kernel32.dll","CreateMutexA",12,(void*)gso_create_mutex},
    {"kernel32.dll","CreateMutexW",12,(void*)gso_create_mutex_w},
    {"kernel32.dll","ReleaseMutex",4,(void*)gso_release_mutex},
    {"kernel32.dll","CreateSemaphoreA",16,(void*)gso_create_semaphore},
    {"kernel32.dll","CreateSemaphoreW",16,(void*)gso_create_semaphore_w},
    {"kernel32.dll","ReleaseSemaphore",12,(void*)gso_release_semaphore},
    {"kernel32.dll","WaitForSingleObject",8,(void*)gso_wait_single},
    {"kernel32.dll","WaitForSingleObjectEx",12,(void*)gso_wait_single_ex},
    {"kernel32.dll","WaitForMultipleObjects",16,(void*)gso_wait_multiple},
    {"kernel32.dll","WaitForMultipleObjectsEx",20,(void*)gso_wait_multiple_ex},
    {"kernel32.dll","InitializeCriticalSection",4,(void*)gso_initialize_critical_section},
    {"kernel32.dll","InitializeCriticalSectionAndSpinCount",8,(void*)gso_initialize_critical_section},
    {"kernel32.dll","DeleteCriticalSection",4,(void*)gso_delete_critical_section},
};
const uint32_t gso_export_count=(uint32_t)(sizeof(gso_exports)/sizeof(gso_exports[0]));
