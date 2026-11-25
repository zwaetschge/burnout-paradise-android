#ifndef BURNOUT_GUEST_TLS_H
#define BURNOUT_GUEST_TLS_H
#include <stdint.h>
enum { GT_SLOTS=1088,GT_MINIMUM=64 };
typedef struct GuestTlsProcess GuestTlsProcess;
typedef struct GuestTlsThread GuestTlsThread;
GuestTlsProcess *gt_process_create(void);
void gt_process_destroy(GuestTlsProcess *p);
GuestTlsThread *gt_thread_create(GuestTlsProcess *p);
void gt_thread_destroy(GuestTlsThread *t);
uint32_t gt_alloc(GuestTlsThread *t);
uint32_t gt_free(GuestTlsThread *t,uint32_t index);
uint32_t gt_get(GuestTlsThread *t,uint32_t index);
uint32_t gt_set(GuestTlsThread *t,uint32_t index,uint32_t value);
uint32_t gt_error(GuestTlsThread *t);
void gt_set_error(GuestTlsThread *t,uint32_t value);
/* Snapshot without changing LastError (for guest TEB synchronization/tests). */
void gt_snapshot(GuestTlsThread *t,uint32_t values[GT_SLOTS]);
#endif
