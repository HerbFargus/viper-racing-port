// w32_time.cpp -- the time stand-ins (w32_kernel.h): QueryPerformanceCounter / Frequency, GetLocalTime, WINMM's
// timeGetTime and timeKillEvent.
//
// The clocks are the OS's: on Windows Windows' own (the session recorder and the port read the same clocks, and a
// recorded session's times came from them); on Linux CLOCK_MONOTONIC, with Windows' performance frequency (10 MHz
// on Windows 10/11 -- ProfBegin keeps freq / 1000 as its ticks per ms), and the local time from localtime_r.
// timeKillEvent: the game never makes a multimedia timer (timeSetEvent isn't among its imports); the one call, in
// task_exception_handler, passes timer periods left over from an older design. No timer exists, so every id is
// refused as Windows refuses an id that isn't a timer: TIMERR_NOCANDO (97).
#include "w32_kernel.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#include <sys/time.h>
#include <time.h>
#endif

int32_t W32K_CALL w32_QueryPerformanceCounter(int64_t* count) {
#ifdef _WIN32
    return QueryPerformanceCounter((LARGE_INTEGER*)count);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *count = (int64_t)ts.tv_sec * 10000000 + ts.tv_nsec / 100;
    return 1;
#endif
}

int32_t W32K_CALL w32_QueryPerformanceFrequency(int64_t* freq) {
#ifdef _WIN32
    return QueryPerformanceFrequency((LARGE_INTEGER*)freq);
#else
    *freq = 10000000;
    return 1;
#endif
}

void W32K_CALL w32_GetLocalTime(W32SystemTime* st) {
#ifdef _WIN32
    static_assert(sizeof(SYSTEMTIME) == sizeof(W32SystemTime), "SYSTEMTIME");
    GetLocalTime((SYSTEMTIME*)st);
#else
    struct timeval tv;
    gettimeofday(&tv, 0);
    const time_t s = tv.tv_sec;
    struct tm t;
    localtime_r(&s, &t);
    st->year = (uint16_t)(t.tm_year + 1900);
    st->month = (uint16_t)(t.tm_mon + 1);
    st->day_of_week = (uint16_t)t.tm_wday;
    st->day = (uint16_t)t.tm_mday;
    st->hour = (uint16_t)t.tm_hour;
    st->minute = (uint16_t)t.tm_min;
    st->second = (uint16_t)(t.tm_sec > 59 ? 59 : t.tm_sec);   // (a leap second: Windows never shows :60)
    st->ms = (uint16_t)(tv.tv_usec / 1000);
#endif
}

uint32_t W32K_CALL w32_timeGetTime() {
#ifdef _WIN32
    return timeGetTime();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
#endif
}

uint32_t W32K_CALL w32_timeKillEvent(uint32_t id) {
    (void)id;
    return 97;                                   // TIMERR_NOCANDO: no such timer
}
