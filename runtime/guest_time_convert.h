#ifndef BURNOUT_GUEST_TIME_CONVERT_H
#define BURNOUT_GUEST_TIME_CONVERT_H
#include "guest_memory.h"
/* SYSTEMTIME / FILETIME conversion and the millisecond tick clock.
 *
 * SYSTEMTIME is the documented 16-byte PE32 layout:
 *   wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds (WORDs).
 * FILETIME is 100 ns units since 1601-01-01 UTC. The Gregorian conversion is
 * exact (proleptic Gregorian, days-from-civil), validated against Python
 * datetime for random values by tools/test_guest_kernel_wave2.py.
 *
 * Time zone: the project profile is UTC, so GetLocalTime and
 * FileTimeToLocalFileTime apply an explicit bias argument (0 for UTC) instead
 * of reading the host zone. No host locale/timezone state leaks into the guest.
 *
 * GetTickCount/GetTickCount64/timeGetTime use the same monotonic boot clock
 * (CLOCK_BOOTTIME, including suspend) as the shared interrupt-time provider.
 *
 * *result receives the Win32 return value; LastError is written to the guest
 * TEB (+0x34). A non-zero function result is a named model stop (GM_*). */
#define GTC_SYSTEMTIME_SIZE 16u
#define GTC_ERROR_INVALID_PARAMETER 87u
#define GTC_EPOCH_TICKS UINT64_C(116444736000000000)
#define GTC_MIN_YEAR 1601u
#define GTC_MAX_YEAR 30827u

typedef struct {const char *dll;const char *name;uint32_t stdcall_arg_bytes;void *fn;} GtcExport;
extern const GtcExport gtc_exports[];
extern const uint32_t gtc_export_count;

/* Pure conversions. Invalid field ranges (month 0/13, day 31 in April, hour
 * 24, year outside 1601..30827, ...) return GM_UNSUPPORTED; the guest wrappers
 * turn that into FALSE + ERROR_INVALID_PARAMETER exactly like Win32. */
int gtc_civil_to_filetime(uint32_t year,uint32_t month,uint32_t day,uint32_t hour,
                          uint32_t minute,uint32_t second,uint32_t milliseconds,
                          uint64_t *filetime);
/* fields = {year,month,day_of_week,day,hour,minute,second,milliseconds}. */
int gtc_filetime_to_civil(uint64_t filetime,uint32_t fields[8]);

int gtc_system_time_to_filetime(GuestMemory *,uint32_t teb,uint32_t system_time,
                                uint32_t filetime,uint32_t *result);
int gtc_filetime_to_system_time(GuestMemory *,uint32_t teb,uint32_t filetime,
                                uint32_t system_time,uint32_t *result);
int gtc_get_system_time(GuestMemory *,uint32_t output,uint32_t *result);
/* Local time = UTC + bias (bias in 100 ns units, 0 for the UTC project profile). */
int gtc_get_local_time(GuestMemory *,uint32_t output,int64_t bias_100ns,uint32_t *result);
int gtc_filetime_to_local_filetime(GuestMemory *,uint32_t teb,uint32_t input,uint32_t output,
                                   int64_t bias_100ns,uint32_t *result);
int gtc_get_tick_count(uint32_t *result);
int gtc_get_tick_count64(GuestMemory *,uint32_t output,uint32_t *result);
/* winmm!timeGetTime: the same monotonic millisecond clock as GetTickCount. */
int gtc_time_get_time(uint32_t *result);
#endif
