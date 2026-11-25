#include "guest_critical.h"

/* A14: multi-thread mode (defined at the end of this file). The single-CPU
 * functions below dispatch to the blocking implementation when the runtime has
 * enabled it; with the mode off they behave exactly as before. */
extern int gcs_multi;
int gcs_enter_mt(GuestMemory *,uint32_t section,uint32_t thread,int try_only,uint32_t *entered);
int gcs_leave_mt(GuestMemory *,uint32_t section,uint32_t thread);
int gcs_delete_mt(GuestMemory *,uint32_t section);
int gcs_initialize_mt(GuestMemory *,uint32_t section,uint32_t spin,uint32_t flags,uint32_t processors);

static uint32_t word(const uint8_t *b){return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);}
static int store(GuestMemory *m,uint32_t address,const uint32_t values[6]){
    uint8_t bytes[24];for(unsigned i=0;i<6;i++)for(unsigned j=0;j<4;j++)bytes[i*4+j]=(uint8_t)(values[i]>>(j*8));
    return gm_write(m,address,bytes,sizeof(bytes));
}
static int load(GuestMemory *m,uint32_t address,uint32_t values[6]){
    int rc=gm_check(m,address,24,GM_READ|GM_WRITE);if(rc)return rc;
    uint8_t bytes[24];rc=gm_read(m,address,bytes,sizeof(bytes));if(rc)return rc;
    for(unsigned i=0;i<6;i++)values[i]=word(bytes+4*i);
    if(values[0]!=UINT32_MAX || values[4] || values[5])return GM_UNSUPPORTED;
    return GM_OK;
}
int gcs_initialize(GuestMemory *m,uint32_t section,uint32_t spin,uint32_t flags,uint32_t processors){
    if(gcs_multi)return gcs_initialize_mt(m,section,spin,flags,processors);
    (void)spin; /* The declared single processor ignores the requested count. */
    if(processors!=1 || (flags!=0 && flags!=0x01000000u))return GM_UNSUPPORTED;
    const uint32_t values[6]={UINT32_MAX,UINT32_MAX,0,0,0,0};
    return store(m,section,values);
}
int gcs_enter(GuestMemory *m,uint32_t section,uint32_t thread,int try_only,uint32_t *entered){
    if(gcs_multi)return gcs_enter_mt(m,section,thread,try_only,entered);
    if(!thread || !entered)return GM_RANGE;
    uint32_t v[6];int rc=load(m,section,v);if(rc)return rc;
    if(!v[2]){
        if(v[1]!=UINT32_MAX || v[3])return GM_UNSUPPORTED;
        v[1]=0;v[2]=1;v[3]=thread;
    }else if(v[3]==thread){
        if(v[2]==UINT32_MAX || v[1]!=v[2]-1)return GM_UNSUPPORTED;
        v[1]++;v[2]++;
    }else{
        if(!try_only)return GM_UNSUPPORTED;
        *entered=0;return GM_OK;
    }
    rc=store(m,section,v);if(!rc)*entered=1;return rc;
}
int gcs_leave(GuestMemory *m,uint32_t section,uint32_t thread){
    if(gcs_multi)return gcs_leave_mt(m,section,thread);
    uint32_t v[6];int rc=load(m,section,v);if(rc)return rc;
    if(!thread || !v[2] || v[3]!=thread || v[1]!=v[2]-1)return GM_UNSUPPORTED;
    v[2]--;v[1]--;if(!v[2])v[3]=0;return store(m,section,v);
}
int gcs_delete(GuestMemory *m,uint32_t section){
    if(gcs_multi)return gcs_delete_mt(m,section);
    uint32_t v[6];int rc=load(m,section,v);if(rc)return rc;
    if(v[2] || v[3] || v[1]!=UINT32_MAX)return GM_UNSUPPORTED;
    v[0]=0;return store(m,section,v);
}

/* ==================================================================
 * A14 -- multi-thread mode
 *
 * The code above is the documented one-guest-CPU model and is left exactly as
 * it was. With real guest threads (runtime/static_threads.c) contention is no
 * longer a modelling error but a normal event, so this table gives every
 * critical-section guest address a host PTHREAD_MUTEX_RECURSIVE and the enter /
 * leave paths block on it. Ownership and recursion are still mirrored into the
 * guest object, so guest code that reads RecursionCount/OwningThread sees the
 * documented values.
 *
 * The table is a small open-addressed map keyed by guest address; it is only
 * touched in multi-thread mode and never stores a host pointer in guest memory.
 * ================================================================== */
#include <pthread.h>
#include <stdlib.h>

#define GCS_HOST_SLOTS 512u

typedef struct {
    uint32_t address;      /* 0 = free */
    pthread_mutex_t mutex;
    int live;
} GcsHostLock;

static GcsHostLock gcs_host[GCS_HOST_SLOTS];
static pthread_mutex_t gcs_host_table_lock = PTHREAD_MUTEX_INITIALIZER;
int gcs_multi;

void gcs_set_multithread(int enabled) {
    pthread_mutex_lock(&gcs_host_table_lock);
    gcs_multi = enabled ? 1 : 0;
    pthread_mutex_unlock(&gcs_host_table_lock);
}
int gcs_multithread(void) { return gcs_multi; }

/* Caller holds gcs_host_table_lock. */
static GcsHostLock *gcs_slot_for(uint32_t address) {
    uint32_t start = (address >> 2) % GCS_HOST_SLOTS;
    GcsHostLock *reuse = NULL;
    for (uint32_t n = 0; n < GCS_HOST_SLOTS; n++) {
        GcsHostLock *s = &gcs_host[(start + n) % GCS_HOST_SLOTS];
        if (s->live && s->address == address) return s;
        if (!s->live && !reuse) reuse = s;
    }
    if (!reuse) return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(&reuse->mutex, &attr)) {
        pthread_mutexattr_destroy(&attr);
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);
    reuse->address = address;
    reuse->live = 1;
    return reuse;
}

static GcsHostLock *gcs_lookup(uint32_t address) {
    GcsHostLock *found = NULL;
    pthread_mutex_lock(&gcs_host_table_lock);
    uint32_t start = (address >> 2) % GCS_HOST_SLOTS;
    for (uint32_t n = 0; n < GCS_HOST_SLOTS; n++) {
        GcsHostLock *s = &gcs_host[(start + n) % GCS_HOST_SLOTS];
        if (s->live && s->address == address) { found = s; break; }
    }
    pthread_mutex_unlock(&gcs_host_table_lock);
    return found;
}

static void gcs_host_release(uint32_t address) {
    pthread_mutex_lock(&gcs_host_table_lock);
    uint32_t start = (address >> 2) % GCS_HOST_SLOTS;
    for (uint32_t n = 0; n < GCS_HOST_SLOTS; n++) {
        GcsHostLock *s = &gcs_host[(start + n) % GCS_HOST_SLOTS];
        if (s->live && s->address == address) {
            pthread_mutex_destroy(&s->mutex);
            s->live = 0;
            s->address = 0;
            break;
        }
    }
    pthread_mutex_unlock(&gcs_host_table_lock);
}

/* Multi-thread enter/leave. Kept separate from the single-CPU functions so the
 * documented model stays byte-identical when the mode is off. */
int gcs_enter_mt(GuestMemory *m, uint32_t section, uint32_t thread, int try_only,
                 uint32_t *entered) {
    /* `entered` is only required by the Try variant; the blocking form is a
     * VOID Win32 call and passes NULL. */
    if (!m || !thread || (try_only && !entered)) return GM_RANGE;
    GcsHostLock *host;
    pthread_mutex_lock(&gcs_host_table_lock);
    host = gcs_slot_for(section);
    pthread_mutex_unlock(&gcs_host_table_lock);
    if (!host) return GM_UNSUPPORTED;
    if (try_only) {
        if (pthread_mutex_trylock(&host->mutex)) { *entered = 0; return GM_OK; }
    } else {
        pthread_mutex_lock(&host->mutex);
    }
    uint32_t v[6];
    int rc = load(m, section, v);
    if (rc) { pthread_mutex_unlock(&host->mutex); return rc; }
    if (!v[2]) {
        v[1] = 0;
        v[2] = 1;
        v[3] = thread;
    } else if (v[3] == thread) {
        if (v[2] == UINT32_MAX) { pthread_mutex_unlock(&host->mutex); return GM_UNSUPPORTED; }
        v[1]++;
        v[2]++;
    } else {
        /* The guest object says another thread owns it, but the host mutex was
         * free: the guest used a different synchronisation primitive on the
         * same bytes. Refuse instead of inventing an owner. */
        pthread_mutex_unlock(&host->mutex);
        return GM_UNSUPPORTED;
    }
    rc = store(m, section, v);
    if (rc) { pthread_mutex_unlock(&host->mutex); return rc; }
    if (entered) *entered = 1;
    return GM_OK;
}

int gcs_leave_mt(GuestMemory *m, uint32_t section, uint32_t thread) {
    if (!m || !thread) return GM_RANGE;
    GcsHostLock *host = gcs_lookup(section);
    if (!host) return GM_UNSUPPORTED;
    uint32_t v[6];
    int rc = load(m, section, v);
    if (rc) return rc;
    if (!v[2] || v[3] != thread || v[1] != v[2] - 1) return GM_UNSUPPORTED;
    v[2]--;
    v[1]--;
    if (!v[2]) v[3] = 0;
    rc = store(m, section, v);
    if (rc) return rc;
    pthread_mutex_unlock(&host->mutex);
    return GM_OK;
}

int gcs_delete_mt(GuestMemory *m, uint32_t section) {
    uint32_t v[6];
    int rc = load(m, section, v);
    if (rc) return rc;
    if (v[2] || v[3] || v[1] != UINT32_MAX) return GM_UNSUPPORTED;
    v[0] = 0;
    rc = store(m, section, v);
    if (rc) return rc;
    gcs_host_release(section);
    return GM_OK;
}

int gcs_initialize_mt(GuestMemory *m, uint32_t section, uint32_t spin, uint32_t flags,
                      uint32_t processors) {
    /* The single-CPU initialiser is the same 24-byte encoding; call it with the
     * mode temporarily off so it cannot dispatch back here. */
    int saved = gcs_multi;
    gcs_multi = 0;
    int rc = gcs_initialize(m, section, spin, flags, processors);
    gcs_multi = saved;
    if (rc) return rc;
    /* A reused guest address must not inherit the previous lock's state. */
    gcs_host_release(section);
    return GM_OK;
}
