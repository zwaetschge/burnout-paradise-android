#define _POSIX_C_SOURCE 200809L
#include "guest_thread_api.h"
#include "guest_sync_objects.h"
#include "guest_handles.h"
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

/* Guest thread objects. All state is guarded by one module mutex; a single
 * module condition variable carries exit waits. A waiter holds a reference so
 * CloseHandle cannot free the object under it. */
typedef struct GtaThread {
    uint32_t handle,thread_id,start,parameter,stack_size,flags;
    uint32_t suspend_count,started,exited,exit_code,dead,refs;
    struct GtaThread *next;
} GtaThread;

static GtaThread *threads;
static pthread_mutex_t gta_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gta_cond;
static pthread_condattr_t gta_attr;
static pthread_once_t gta_once=PTHREAD_ONCE_INIT;
static GtaThreadLauncher launcher;
static GtaThreadExitHook exit_hook;
static uint32_t next_thread_id=0x1008u;

/* Exit waits use CLOCK_MONOTONIC deadlines, so the condition variable must use
 * the same clock instead of the default CLOCK_REALTIME. */
static void init_cond_clock(void){
    pthread_condattr_init(&gta_attr);
    pthread_condattr_setclock(&gta_attr,CLOCK_MONOTONIC);
    pthread_cond_init(&gta_cond,&gta_attr);
}
static void ensure_cond_clock(void){pthread_once(&gta_once,init_cond_clock);}
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
static void thread_destroy(void *p){
    GtaThread *t=p;int last;
    pthread_mutex_lock(&gta_lock);
    t->dead=1;
    GtaThread **slot=&threads;while(*slot && *slot!=t)slot=&(*slot)->next;
    if(*slot)*slot=t->next;
    last=(--t->refs==0);
    pthread_mutex_unlock(&gta_lock);
    if(last)free(t);
}
/* Caller holds gta_lock. */
static void drop_reference(GtaThread *t){
    if(--t->refs==0)free(t);
}
/* Returns a referenced thread object, or NULL. */
static GtaThread *acquire(uint32_t handle){
    GtaThread *t=NULL;
    pthread_mutex_lock(&gta_lock);
    void *object=gh_lookup(handle,GH_KIND_THREAD,NULL);
    if(object){t=object;t->refs++;}
    pthread_mutex_unlock(&gta_lock);
    return t;
}
static void mark_exited(GtaThread *t,uint32_t code){
    /* Caller holds gta_lock. */
    ensure_cond_clock();
    if(t->exited)return;
    t->exited=1;t->exit_code=code;
    pthread_cond_broadcast(&gta_cond);
}
static void report_exit(GtaThread *t,uint32_t code){
    uint32_t id=t->thread_id;
    pthread_mutex_lock(&gta_lock);mark_exited(t,code);pthread_mutex_unlock(&gta_lock);
    /* A mutex owned by the exiting thread becomes abandoned for the next
     * waiter, matching the documented Win32 contract. */
    gso_abandon_thread(NULL,id);
    if(exit_hook)exit_hook(id,code);
}
void gta_set_thread_launcher(GtaThreadLauncher fn){
    pthread_mutex_lock(&gta_lock);launcher=fn;pthread_mutex_unlock(&gta_lock);
}
void gta_set_thread_exit_hook(GtaThreadExitHook fn){
    pthread_mutex_lock(&gta_lock);exit_hook=fn;pthread_mutex_unlock(&gta_lock);
}
int gta_launcher_present(void){
    int present;pthread_mutex_lock(&gta_lock);present=launcher!=NULL;pthread_mutex_unlock(&gta_lock);
    return present;
}
void gta_set_thread_id_base(uint32_t base){
    pthread_mutex_lock(&gta_lock);if(base)next_thread_id=base;pthread_mutex_unlock(&gta_lock);
}
/* Starts a guest thread through the runtime launcher. Caller holds no lock. */
static int start_thread(GtaThread *t){
    GtaThreadLauncher fn;
    pthread_mutex_lock(&gta_lock);
    if(t->started || t->dead){pthread_mutex_unlock(&gta_lock);return GM_UNSUPPORTED;}
    if(t->suspend_count){pthread_mutex_unlock(&gta_lock);return GM_UNSUPPORTED;}
    fn=launcher;
    if(!fn){pthread_mutex_unlock(&gta_lock);return GTA_STOP_NO_LAUNCHER;}
    t->started=1;
    uint32_t id=t->thread_id,start=t->start,parameter=t->parameter,stack=t->stack_size;
    pthread_mutex_unlock(&gta_lock);
    int rc=fn(id,start,parameter,stack);
    if(rc){
        pthread_mutex_lock(&gta_lock);t->started=0;pthread_mutex_unlock(&gta_lock);
        return rc;
    }
    return GM_OK;
}
int gta_create_thread(GuestMemory *memory,uint32_t teb,uint32_t attributes,uint32_t stack_size,
                      uint32_t start,uint32_t parameter,uint32_t flags,
                      uint32_t *handle,uint32_t thread_id){
    if(!memory || !handle)return GM_UNSUPPORTED;
    /* SECURITY_ATTRIBUTES, thread-desktop and every other creation flag are
     * outside this model; only CREATE_SUSPENDED is honoured. */
    if(attributes || (flags&~GTA_CREATE_SUSPENDED))return GM_UNSUPPORTED;
    if(!start)return GM_UNSUPPORTED;
    int rc=gm_check(memory,start,1,GM_EXECUTE);if(rc)return rc;
    if(!(flags&GTA_CREATE_SUSPENDED) && !gta_launcher_present())return GTA_STOP_NO_LAUNCHER;
    /* A thread handle is waitable; install the sync hook on first use. */
    gso_set_thread_wait_hook(gta_wait_thread);
    GtaThread *t=calloc(1,sizeof(*t));
    if(!t)return GM_UNSUPPORTED;
    t->start=start;t->parameter=parameter;t->stack_size=stack_size;t->flags=flags;
    t->suspend_count=(flags&GTA_CREATE_SUSPENDED)?1u:0u;t->refs=1;
    pthread_mutex_lock(&gta_lock);
    t->thread_id=next_thread_id;next_thread_id+=4;
    uint32_t id=t->thread_id;
    pthread_mutex_unlock(&gta_lock);
    uint32_t h=gh_insert(GH_KIND_THREAD,t,thread_destroy);
    if(!h){
        free(t);
        rc=last_error(memory,teb,GTA_ERROR_NOT_ENOUGH_MEMORY);if(rc)return rc;
        *handle=0;return GM_OK;
    }
    t->handle=h;
    pthread_mutex_lock(&gta_lock);t->next=threads;threads=t;pthread_mutex_unlock(&gta_lock);
    if(!(flags&GTA_CREATE_SUSPENDED)){
        rc=start_thread(t);
        if(rc){gh_close(h);return rc;}
    }
    if(thread_id){
        uint8_t bytes[4];put32(bytes,id);
        rc=gm_write(memory,thread_id,bytes,4);
        if(rc){gh_close(h);return rc;}
    }
    *handle=h;return GM_OK;
}
int gta_resume_thread(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    GtaThread *t=acquire(handle);
    if(!t){
        int rc=last_error(memory,teb,GTA_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=UINT32_MAX;return GM_OK;
    }
    pthread_mutex_lock(&gta_lock);
    uint32_t previous=t->suspend_count;
    int start=(!t->started && !t->dead && previous==1);
    if(!t->dead && previous>1)t->suspend_count=previous-1;
    else if(start)t->suspend_count=0;
    pthread_mutex_unlock(&gta_lock);
    int rc=GM_OK;
    if(start)rc=start_thread(t);
    pthread_mutex_lock(&gta_lock);drop_reference(t);pthread_mutex_unlock(&gta_lock);
    if(rc)return rc;
    *result=previous;return GM_OK;
}
int gta_suspend_thread(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    GtaThread *t=acquire(handle);
    if(!t){
        int rc=last_error(memory,teb,GTA_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=UINT32_MAX;return GM_OK;
    }
    int rc=GM_OK;uint32_t previous=0;
    pthread_mutex_lock(&gta_lock);
    /* A guest CPU that already ran cannot be suspended by this model. */
    if(t->dead || t->started)rc=GM_UNSUPPORTED;
    else{previous=t->suspend_count;t->suspend_count++;}
    drop_reference(t);
    pthread_mutex_unlock(&gta_lock);
    if(rc)return rc;
    *result=previous;return GM_OK;
}
int gta_get_exit_code_thread(GuestMemory *memory,uint32_t teb,uint32_t handle,uint32_t code,
                             uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    GtaThread *t=acquire(handle);
    if(!t){
        int rc=last_error(memory,teb,GTA_ERROR_INVALID_HANDLE);if(rc)return rc;
        *result=0;return GM_OK;
    }
    uint32_t value;
    pthread_mutex_lock(&gta_lock);
    value=t->exited?t->exit_code:GTA_STILL_ACTIVE;
    drop_reference(t);
    pthread_mutex_unlock(&gta_lock);
    if(code){
        uint8_t bytes[4];put32(bytes,value);
        int rc=gm_write(memory,code,bytes,4);if(rc)return rc;
    }
    *result=1;return GM_OK;
}
int gta_exit_thread(GuestMemory *memory,uint32_t teb,uint32_t exit_code,uint32_t *result){
    if(!memory || !result)return GM_UNSUPPORTED;
    uint32_t id;int rc=current_thread(memory,teb,&id);if(rc)return rc;
    GtaThread *found=NULL;
    pthread_mutex_lock(&gta_lock);
    for(GtaThread *t=threads;t;t=t->next)if(t->thread_id==id && !t->dead){found=t;found->refs++;break;}
    pthread_mutex_unlock(&gta_lock);
    if(found){
        report_exit(found,exit_code);
        pthread_mutex_lock(&gta_lock);drop_reference(found);pthread_mutex_unlock(&gta_lock);
    }else{
        /* Exiting the runtime's main thread: no thread object exists. */
        gso_abandon_thread(NULL,id);
        if(exit_hook)exit_hook(id,exit_code);
    }
    /* ExitThread never returns to the guest. The runtime must end the guest
     * thread; this is a named stop, not a fabricated Win32 return value. */
    return GTA_STOP_THREAD_EXIT;
}
static int sleep_milliseconds(uint32_t milliseconds){
    struct timespec remaining;
    remaining.tv_sec=(time_t)(milliseconds/1000u);
    remaining.tv_nsec=(long)((milliseconds%1000u)*1000000u);
    while(nanosleep(&remaining,&remaining)){
        if(errno!=EINTR)return GM_UNSUPPORTED;
    }
    return GM_OK;
}
int gta_sleep(uint32_t milliseconds,uint32_t *result){
    if(!result)return GM_UNSUPPORTED;
    if(milliseconds==UINT32_MAX)return GM_UNSUPPORTED; /* Sleep(INFINITE) */
    int rc=sleep_milliseconds(milliseconds);if(rc)return rc;
    *result=0;return GM_OK;
}
int gta_sleep_ex(uint32_t milliseconds,uint32_t alertable,uint32_t *result){
    if(!result)return GM_UNSUPPORTED;
    /* Alertable: the sleep ends early only when an APC is queued to this
     * thread. Nothing in this runtime can queue one (QueueUserAPC, ReadFileEx/
     * WriteFileEx completion routines and waitable-timer routines are not
     * provided; calling any of them is a named stop), so the queue is always
     * empty and the call is exactly a plain sleep that returns 0. */
    (void)alertable;
    if(milliseconds==UINT32_MAX)return GM_UNSUPPORTED;
    int rc=sleep_milliseconds(milliseconds);if(rc)return rc;
    *result=0;return GM_OK;
}
int gta_switch_to_thread(uint32_t *result){
    if(!result)return GM_UNSUPPORTED;
    if(sched_yield())return GM_UNSUPPORTED;
    /* Documented contract: non-zero means a thread switch was performed. */
    *result=1;return GM_OK;
}
int gta_note_thread_exit(uint32_t handle,uint32_t exit_code){
    GtaThread *t=acquire(handle);
    if(!t)return GTA_STOP_INVALID_THREAD;
    report_exit(t,exit_code);
    pthread_mutex_lock(&gta_lock);drop_reference(t);pthread_mutex_unlock(&gta_lock);
    return GM_OK;
}
/* DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
 * ...): a new real handle to the calling thread (documented way to turn the
 * thread pseudo-handle into one other threads can use). When a handle object
 * of this thread is still open the new handle shares it; otherwise (the
 * runtime's main thread has none, and a creator may already have closed the
 * CreateThread handle) a thread object for the running thread is made. Its
 * exit is reported by thread id (gta_note_thread_exit_id). */
int gta_open_current_thread(GuestMemory *memory,uint32_t teb,uint32_t *handle){
    if(!memory || !handle)return GM_UNSUPPORTED;
    uint32_t id;int rc=current_thread(memory,teb,&id);if(rc)return rc;
    uint32_t existing=0;
    pthread_mutex_lock(&gta_lock);
    for(GtaThread *t=threads;t;t=t->next)
        if(t->thread_id==id && !t->dead && !t->exited){existing=t->handle;break;}
    pthread_mutex_unlock(&gta_lock);
    if(existing){
        uint32_t h=gh_duplicate(existing);
        if(h){*handle=h;return GM_OK;}
    }
    gso_set_thread_wait_hook(gta_wait_thread);
    GtaThread *t=calloc(1,sizeof(*t));
    if(!t)return GM_UNSUPPORTED;
    t->thread_id=id;t->started=1;t->refs=1;
    uint32_t h=gh_insert(GH_KIND_THREAD,t,thread_destroy);
    if(!h){free(t);*handle=0;return GM_OK;}       /* table full: caller reports the failure */
    t->handle=h;
    pthread_mutex_lock(&gta_lock);t->next=threads;threads=t;pthread_mutex_unlock(&gta_lock);
    *handle=h;return GM_OK;
}
/* Marks every thread object of `thread_id` as exited (handles opened through
 * gta_open_current_thread are not the handle the scheduler knows). */
void gta_note_thread_exit_id(uint32_t thread_id,uint32_t exit_code){
    pthread_mutex_lock(&gta_lock);
    for(GtaThread *t=threads;t;t=t->next)
        if(t->thread_id==thread_id && !t->dead)mark_exited(t,exit_code);
    pthread_mutex_unlock(&gta_lock);
}
int gta_wait_thread(uint32_t handle,uint32_t milliseconds,uint32_t *result){
    if(!result)return GM_UNSUPPORTED;
    ensure_cond_clock();
    GtaThread *t=acquire(handle);
    if(!t)return GTA_STOP_INVALID_THREAD;
    struct timespec limit;int rc=GM_OK;uint32_t value=GSO_WAIT_OBJECT_0;
    if(milliseconds!=GSO_INFINITE){
        rc=deadline_after(milliseconds,&limit);
        if(rc){pthread_mutex_lock(&gta_lock);drop_reference(t);pthread_mutex_unlock(&gta_lock);return rc;}
    }
    pthread_mutex_lock(&gta_lock);
    while(!t->exited && !t->dead){
        int err;
        if(milliseconds==GSO_INFINITE)err=pthread_cond_wait(&gta_cond,&gta_lock);
        else err=pthread_cond_timedwait(&gta_cond,&gta_lock,&limit);
        if(err==ETIMEDOUT){value=GSO_WAIT_TIMEOUT;break;}
        if(err){rc=GM_UNSUPPORTED;break;}
    }
    if(!rc && t->dead)rc=GM_UNSUPPORTED;
    drop_reference(t);
    pthread_mutex_unlock(&gta_lock);
    if(rc)return rc;
    *result=value;return GM_OK;
}
uint32_t gta_thread_count(void){
    uint32_t n=0;pthread_mutex_lock(&gta_lock);
    for(GtaThread *t=threads;t;t=t->next)n++;
    pthread_mutex_unlock(&gta_lock);return n;
}
void gta_reset_for_tests(void){
    for(uint32_t guard=0;threads && guard<GH_MAX_HANDLES;guard++){
        pthread_mutex_lock(&gta_lock);
        uint32_t handle=threads->handle;
        pthread_mutex_unlock(&gta_lock);
        if(!gh_close(handle))break;
    }
    pthread_mutex_lock(&gta_lock);next_thread_id=0x1008u;pthread_mutex_unlock(&gta_lock);
}
/* Wiring list for runtime/win32_api_table.c. */
const GtaExport gta_exports[]={
    {"kernel32.dll","CreateThread",24,(void*)gta_create_thread},
    {"kernel32.dll","ResumeThread",4,(void*)gta_resume_thread},
    {"kernel32.dll","SuspendThread",4,(void*)gta_suspend_thread},
    {"kernel32.dll","GetExitCodeThread",8,(void*)gta_get_exit_code_thread},
    {"kernel32.dll","ExitThread",4,(void*)gta_exit_thread},
    {"kernel32.dll","Sleep",4,(void*)gta_sleep},
    {"kernel32.dll","SleepEx",8,(void*)gta_sleep_ex},
    {"kernel32.dll","SwitchToThread",0,(void*)gta_switch_to_thread},
};
const uint32_t gta_export_count=(uint32_t)(sizeof(gta_exports)/sizeof(gta_exports[0]));
