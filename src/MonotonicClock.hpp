#pragma once
#include <cstdint>
#ifdef __APPLE__
#include <mach/mach_time.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <chrono>
#endif

namespace separate_song {
// Use the same system clock as OBS and the effects tap, across processes.
inline uint64_t monotonicNs() {
#ifdef __APPLE__
    static const auto timebase=[] { mach_timebase_info_data_t t{};mach_timebase_info(&t);return t; }();
    auto t=mach_absolute_time();
    return (t/timebase.denom)*timebase.numer+(t%timebase.denom)*timebase.numer/timebase.denom;
#elif defined(_WIN32)
    static const auto frequency=[] { LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return uint64_t(f.QuadPart); }();
    LARGE_INTEGER value{};QueryPerformanceCounter(&value);auto t=uint64_t(value.QuadPart);
    return (t/frequency)*1000000000+(t%frequency)*1000000000/frequency;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
}
