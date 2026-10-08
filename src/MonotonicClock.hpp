#pragma once
#include <cstdint>
#include <chrono>
#ifdef _WIN32
#include "Socket.hpp"
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#endif
inline uint64_t monotonicNowNs() {
#ifdef _WIN32
    static const uint64_t frequency=[] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return uint64_t(f.QuadPart); }();
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); auto t=uint64_t(value.QuadPart);
    return t/frequency*1000000000+(t%frequency)*1000000000/frequency;
#elif defined(__APPLE__)
    static const auto base=[] { mach_timebase_info_data_t b{}; mach_timebase_info(&b); return b; }();
    auto t=mach_absolute_time(); return t/base.denom*base.numer+(t%base.denom)*base.numer/base.denom;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
inline uint64_t processSessionID() {
    static const uint64_t session=monotonicNowNs() ^ uint64_t(reinterpret_cast<uintptr_t>(&processSessionID));
    return session ? session : 1;
}
