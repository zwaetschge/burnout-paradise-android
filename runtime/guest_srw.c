#include "guest_srw.h"

/* A14: multi-thread mode (defined at the end of this file). */
extern int gsrw_multi;
int gsrw_call_mt(GuestSrw *,GuestMemory *,uint32_t operation,uint32_t address,uint32_t *result);

/* Only the single-CPU uncontended encoding is modelled. */
enum { SRW_UNOWNED = 0, SRW_EXCLUSIVE = 1 };
static uint32_t word(const uint8_t *b){
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static void put32(uint8_t *b,uint32_t value){
    b[0]=(uint8_t)value;b[1]=(uint8_t)(value>>8);b[2]=(uint8_t)(value>>16);b[3]=(uint8_t)(value>>24);
}
void gsrw_initialize(GuestSrw *srw){if(srw)srw->operations=0;}
uint32_t gsrw_operations(const GuestSrw *srw){return srw ? srw->operations : 0;}
/* The lock must be a writable, aligned 4-byte guest object. */
static int load(GuestMemory *memory,uint32_t address,uint32_t *value){
    if((address&3u) || (uint64_t)address+4>UINT64_C(0x100000000))return GM_RANGE;
    int rc=gm_check(memory,address,4,GM_READ|GM_WRITE);if(rc)return rc;
    uint8_t bytes[4];rc=gm_read(memory,address,bytes,4);if(rc)return rc;
    *value=word(bytes);return GM_OK;
}
static int store(GuestSrw *srw,GuestMemory *memory,uint32_t address,uint32_t value){
    uint8_t bytes[4];put32(bytes,value);
    int rc=gm_write(memory,address,bytes,4);if(rc)return rc;
    if(srw)srw->operations++;
    return GM_OK;
}
int gsrw_call(GuestSrw *srw,GuestMemory *memory,uint32_t operation,uint32_t address,uint32_t *result){
    if(gsrw_multi)return gsrw_call_mt(srw,memory,operation,address,result);
    if(!memory)return GM_UNSUPPORTED;
    uint32_t value;
    int rc=load(memory,address,&value);if(rc)return rc;
    uint32_t exclusive=value&1u,shared=value&~1u;
    switch(operation){
    case GSRW_ACQUIRE_EXCLUSIVE:
    case GSRW_TRY_ACQUIRE_EXCLUSIVE:
        if(exclusive || shared){
            if(operation==GSRW_TRY_ACQUIRE_EXCLUSIVE){
                if(result)*result=0;return GM_OK;
            }
            return GM_UNSUPPORTED; /* contended wait is not modelled */
        }
        rc=store(srw,memory,address,SRW_EXCLUSIVE);if(rc)return rc;
        if(operation==GSRW_TRY_ACQUIRE_EXCLUSIVE && result)*result=1;
        return GM_OK;
    case GSRW_ACQUIRE_SHARED:
    case GSRW_TRY_ACQUIRE_SHARED:
        if(exclusive){
            if(operation==GSRW_TRY_ACQUIRE_SHARED){
                if(result)*result=0;return GM_OK;
            }
            return GM_UNSUPPORTED; /* shared acquire under an exclusive owner */
        }
        rc=store(srw,memory,address,shared+2u);if(rc)return rc;
        if(operation==GSRW_TRY_ACQUIRE_SHARED && result)*result=1;
        return GM_OK;
    case GSRW_RELEASE_EXCLUSIVE:
        if(value!=SRW_EXCLUSIVE)return GM_UNSUPPORTED;
        return store(srw,memory,address,SRW_UNOWNED);
    case GSRW_RELEASE_SHARED:
        if(exclusive || !shared)return GM_UNSUPPORTED;
        return store(srw,memory,address,shared-2u);
    default:
        return GM_UNSUPPORTED;
    }
}

/* ==================================================================
 * A14 -- multi-thread mode
 *
 * One host pthread_rwlock_t per SRW guest address. The guest's 4-byte object
 * stays a faithful mirror of the documented encoding (bit 0 exclusive, upper
 * bits shared count) so guest code that inspects it sees the same values; the
 * mirror is updated under a module mutex because two guest threads can hold the
 * read lock at the same time. The lock itself does the blocking.
 * ================================================================== */
#include <pthread.h>
#include <errno.h>

#define GSRW_HOST_SLOTS 512u

typedef struct {
    uint32_t address;      /* 0 = free */
    pthread_rwlock_t lock;
    int live;
} GsrwHostLock;

static GsrwHostLock gsrw_host[GSRW_HOST_SLOTS];
static pthread_mutex_t gsrw_table_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gsrw_mirror_lock = PTHREAD_MUTEX_INITIALIZER;
int gsrw_multi;

void gsrw_set_multithread(int enabled) {
    pthread_mutex_lock(&gsrw_table_lock);
    gsrw_multi = enabled ? 1 : 0;
    pthread_mutex_unlock(&gsrw_table_lock);
}
int gsrw_multithread(void) { return gsrw_multi; }

/* Caller holds gsrw_table_lock. */
static GsrwHostLock *gsrw_slot_for(uint32_t address) {
    uint32_t start = (address >> 2) % GSRW_HOST_SLOTS;
    GsrwHostLock *reuse = NULL;
    for (uint32_t n = 0; n < GSRW_HOST_SLOTS; n++) {
        GsrwHostLock *s = &gsrw_host[(start + n) % GSRW_HOST_SLOTS];
        if (s->live && s->address == address) return s;
        if (!s->live && !reuse) reuse = s;
    }
    if (!reuse) return NULL;
    if (pthread_rwlock_init(&reuse->lock, NULL)) return NULL;
    reuse->address = address;
    reuse->live = 1;
    return reuse;
}

static int mirror_update(GuestMemory *memory, uint32_t address, int exclusive_delta,
                         int shared_delta, uint32_t *value_out) {
    pthread_mutex_lock(&gsrw_mirror_lock);
    uint32_t value;
    int rc = load(memory, address, &value);
    if (!rc) {
        uint32_t exclusive = value & 1u, shared = value & ~1u;
        if (exclusive_delta > 0) exclusive = 1;
        if (exclusive_delta < 0) exclusive = 0;
        if (shared_delta > 0) shared += (uint32_t)shared_delta * 2u;
        if (shared_delta < 0) shared -= (uint32_t)(-shared_delta) * 2u;
        rc = store(NULL, memory, address, exclusive | shared);
        if (!rc && value_out) *value_out = exclusive | shared;
    }
    pthread_mutex_unlock(&gsrw_mirror_lock);
    return rc;
}

int gsrw_call_mt(GuestSrw *srw, GuestMemory *memory, uint32_t operation,
                 uint32_t address, uint32_t *result) {
    (void)srw;   /* the multi-thread path does not use the model counter */
    if (!memory) return GM_UNSUPPORTED;
    GsrwHostLock *host;
    pthread_mutex_lock(&gsrw_table_lock);
    host = gsrw_slot_for(address);
    pthread_mutex_unlock(&gsrw_table_lock);
    if (!host) return GM_UNSUPPORTED;
    int rc;
    switch (operation) {
    case GSRW_ACQUIRE_EXCLUSIVE:
        if (pthread_rwlock_wrlock(&host->lock)) return GM_UNSUPPORTED;
        rc = mirror_update(memory, address, 1, 0, NULL);
        if (rc) pthread_rwlock_unlock(&host->lock);
        return rc;
    case GSRW_TRY_ACQUIRE_EXCLUSIVE: {
        int err = pthread_rwlock_trywrlock(&host->lock);
        if (err == EBUSY) { if (result) *result = 0; return GM_OK; }
        if (err) return GM_UNSUPPORTED;
        rc = mirror_update(memory, address, 1, 0, NULL);
        if (rc) { pthread_rwlock_unlock(&host->lock); return rc; }
        if (result) *result = 1;
        return GM_OK;
    }
    case GSRW_ACQUIRE_SHARED:
        if (pthread_rwlock_rdlock(&host->lock)) return GM_UNSUPPORTED;
        rc = mirror_update(memory, address, 0, 1, NULL);
        if (rc) pthread_rwlock_unlock(&host->lock);
        return rc;
    case GSRW_TRY_ACQUIRE_SHARED: {
        int err = pthread_rwlock_tryrdlock(&host->lock);
        if (err == EBUSY) { if (result) *result = 0; return GM_OK; }
        if (err) return GM_UNSUPPORTED;
        rc = mirror_update(memory, address, 0, 1, NULL);
        if (rc) { pthread_rwlock_unlock(&host->lock); return rc; }
        if (result) *result = 1;
        return GM_OK;
    }
    case GSRW_RELEASE_EXCLUSIVE:
        rc = mirror_update(memory, address, -1, 0, NULL);
        if (rc) return rc;
        return pthread_rwlock_unlock(&host->lock) ? GM_UNSUPPORTED : GM_OK;
    case GSRW_RELEASE_SHARED:
        rc = mirror_update(memory, address, 0, -1, NULL);
        if (rc) return rc;
        return pthread_rwlock_unlock(&host->lock) ? GM_UNSUPPORTED : GM_OK;
    default:
        return GM_UNSUPPORTED;
    }
}
