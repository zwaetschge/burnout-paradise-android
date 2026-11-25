#ifndef BURNOUT_GUEST_SYSTEM_TIME_H
#define BURNOUT_GUEST_SYSTEM_TIME_H
#include <stdint.h>
#include "guest_memory.h"
#define GST_OFFSET 0x14u
#define GST_SIZE 12u
#define GST_ZONE_OFFSET 0x20u
#define GST_INTERRUPT_OFFSET 0x08u
#define GST_EPOCH_TICKS UINT64_C(116444736000000000)
typedef struct {
    uint64_t start,step,reads;
    uint32_t diagnostic;
    int64_t timezone_bias;
    uint64_t interrupt_start;
} GuestSystemTime;
/* Production UTC clock; Windows FILETIME units, independently of QPC. */
int gst_now(uint64_t *filetime);
/* Biased elapsed time since boot, including suspend, in 100ns units. */
int gst_interrupt_now(uint64_t *ticks);
/* Value for the read most recently consumed by gst_next; does not advance. */
int gst_interrupt_value(const GuestSystemTime *,uint64_t *ticks);
/* Bias is UTC minus local time, in signed 100ns units. */
int gst_timezone_bias(uint64_t filetime,int64_t *bias);
/* Diagnostic mode consumes explicit common inputs. Production samples realtime.
 * reads is part of rollback/snapshot state, not an irreversible host side effect. */
int gst_next(GuestSystemTime *,uint64_t *filetime);
/* FILETIME API output is two DWORDs, not the SharedUserData three-word form. */
int gst_filetime_value(GuestMemory *,uint32_t output,uint64_t filetime);
/* Single guest CPU: publish LowPart/High1Time/High2Time before its next read.
 * A future concurrent guest runtime needs synchronized publication. */
void gst_store(uint8_t *page,uint64_t filetime);
void gst_store_bias(uint8_t *page,int64_t bias);
void gst_store_interrupt(uint8_t *page,uint64_t ticks);
#endif
