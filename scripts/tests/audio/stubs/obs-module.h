#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif
#define OBS_DECLARE_MODULE()
#define MODULE_EXPORT
#define LOG_ERROR 1
#define LOG_INFO 2
#define OBS_TEXT_INFO 0
#define OBS_SOURCE_TYPE_INPUT 0
#define OBS_SOURCE_DO_NOT_DUPLICATE 2
#define OBS_SOURCE_AUDIO 1
#define OBS_ICON_TYPE_AUDIO_OUTPUT 0
#define SPEAKERS_STEREO 0
#define AUDIO_FORMAT_FLOAT 0
struct obs_source_t {};
struct obs_data_t {};
struct obs_properties_t {};
struct obs_source_audio { const uint8_t* data[8]{}; uint32_t frames{}, speakers{}, format{}, samples_per_sec{}; uint64_t timestamp{}; };
struct obs_source_info {
 const char* id; int type, output_flags, icon_type;
 const char* (*get_name)(void*);
 void* (*create)(obs_data_t*,obs_source_t*);
 void (*destroy)(void*);
 obs_properties_t* (*get_properties)(void*);
 void (*activate)(void*); void (*deactivate)(void*);
};
inline void blog(int,const char*,...) {}
inline obs_properties_t* obs_properties_create() { return nullptr; }
inline void obs_properties_add_text(obs_properties_t*,const char*,const char*,int) {}
inline void obs_register_source(obs_source_info*) {}
inline uint64_t os_gettime_ns() {
#ifdef _WIN32
 LARGE_INTEGER value,frequency; QueryPerformanceCounter(&value); QueryPerformanceFrequency(&frequency);
 return uint64_t(value.QuadPart/frequency.QuadPart)*1000000000 + uint64_t(value.QuadPart%frequency.QuadPart)*1000000000/frequency.QuadPart;
#elif defined(__APPLE__)
 static mach_timebase_info_data_t timebase=[] { mach_timebase_info_data_t t{};mach_timebase_info(&t);return t; }();
 auto ticks=mach_absolute_time();
 return ticks/timebase.denom*timebase.numer + ticks%timebase.denom*timebase.numer/timebase.denom;
#else
 timespec time{};clock_gettime(CLOCK_MONOTONIC,&time);
 return uint64_t(time.tv_sec)*1000000000+time.tv_nsec;
#endif
}
struct Captured {uint64_t timestamp,wall; std::vector<float> samples;};
inline std::mutex capturedMutex;
inline std::vector<Captured> captured;
inline void obs_source_output_audio(obs_source_t*, const obs_source_audio* a) {
 auto p=reinterpret_cast<const float*>(a->data[0]);
 std::lock_guard lock(capturedMutex);captured.push_back({a->timestamp,os_gettime_ns(),{p,p+a->frames*2}});
}
