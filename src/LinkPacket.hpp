#pragma once
#include <cstdint>
#include <cstdlib>
#pragma pack(push, 1)
struct SongLinkPacket {
    char magic[8] = {'G','D','S','O','N','G','1',0};
    uint32_t version = 4;
    uint32_t flags = 0;
    uint32_t epoch = 0;
    int32_t attempt = 0;
    double position = 0;
    double rate = 1;
    double offset = 0;
    char status[24]{};
    char level[96]{};
    char song[96]{};
    char path[1024]{};
    float musicVolume = 1.f;
    float effectsVolume = 1.f;
    uint64_t timestamp = 0; // Monotonic nanoseconds when position/state were sampled.
};
#pragma pack(pop)
inline constexpr unsigned SONG_LINK_V2_SIZE = 1288;
inline constexpr unsigned SONG_LINK_V3_SIZE = 1296;
static_assert(sizeof(SongLinkPacket) == 1304);
inline constexpr int SONG_LINK_PORT = 39022;
inline int receiverPort() {
    auto value = std::getenv("OBS_JUKEBOX_TEST_PORT");
    if (!value) return SONG_LINK_PORT;
    char* end = nullptr;
    auto port = std::strtol(value, &end, 10);
    return end != value && *end == '\0' && port >= 1024 && port <= 65535 ? int(port) : SONG_LINK_PORT;
}

#pragma pack(push, 1)
struct EffectsPacket {
    char magic[8] = {'G','D','S','F','X','1',0,0};
    uint32_t version = 1, sequence = 0, sampleRate = 44100, frames = 0;
    uint64_t timestamp = 0;
    uint32_t stream = 0;
    float samples[512*2]{};
};
#pragma pack(pop)
static_assert(sizeof(EffectsPacket) == 4132);
