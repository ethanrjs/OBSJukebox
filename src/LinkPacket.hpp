#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#pragma pack(push, 1)
struct SongFadePoint {
    uint64_t offsetNs = 0;
    float gain = 1.f;
};
// One FMOD pitch shifter on the path from a sound to the speakers, in signal order. `instance` is
// the DSP address: a mod that destroys and recreates its shifter gets fresh state in OBS. `pitch`
// and `fft` are the parameters as FMOD reads them back.
struct PitchStage {
    float pitch = 0;
    uint16_t fft = 0;
    uint16_t reserved = 0;
    uint64_t instance = 0;
};
struct SongLinkPacket {
    char magic[8] = {'G', 'D', 'S', 'O', 'N', 'G', '1', 0};
    uint32_t version = 5;
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
    uint64_t timestamp = 0;
    uint64_t sessionID = 0;
    int32_t channelID = 0;
    float triggerGain = 1.f;
    uint32_t fadeCount = 0;
    SongFadePoint fades[8]{};
    double loopStart = 0, loopEnd = 0;
};
struct ClockSyncPacket {
    char magic[8] = {'G', 'D', 'C', 'L', 'K', '1', 0, 0};
    uint32_t version = 1, kind = 1;
    uint64_t sessionID = 0, t1 = 0, t2 = 0, t3 = 0;
};
struct EffectsPacket {
    char magic[8] = {'G', 'D', 'S', 'F', 'X', '1', 0, 0};
    uint32_t version = 1, sequence = 0, sampleRate = 44100, frames = 0;
    uint64_t timestamp = 0;
    uint32_t stream = 0;
    float samples[512 * 2]{};
};
struct EffectsPacketV2 {
    char magic[8] = {'G', 'D', 'S', 'F', 'X', '2', 0, 0};
    uint32_t version = 2, sequence = 0, sampleRate = 44100, frames = 0;
    uint64_t timestamp = 0;
    uint32_t stream = 0;
    uint64_t sessionID = 0;
    float samples[512 * 2]{};
};
inline constexpr int SONG_LINK_MAX_CHANNELS = 64;
// FMOD pitch shifter DSPs (such as Mega Hack's Pitch Shifter) that other mods put on the path from a
// sound to GD's output. OBS never hears that output: it decodes the music itself, and the effects tap
// sits upstream of them. Plugins that don't know this magic ignore it.
struct PitchShift {
    float pitch = 1.f;     // product of every active pitch shifter on the path; 1 for none
    uint32_t fftSize = 0;  // the first shifter's FFT size; 0 for none
};
struct PitchPacket {
    char magic[8] = {'G', 'D', 'P', 'I', 'T', 'C', 'H', 0};
    uint32_t version = 1, count = 0;
    uint64_t sessionID = 0, timestamp = 0;
    PitchShift effects;
    struct Voice {
        int32_t channelID = 0;
        PitchShift shift;
    } voices[SONG_LINK_MAX_CHANNELS]{};
};
#pragma pack(pop)
inline constexpr unsigned SONG_LINK_V2_SIZE = 1288, SONG_LINK_V3_SIZE = 1296, SONG_LINK_V4_SIZE = 1304;
static_assert(sizeof(PitchStage) == 16 && alignof(PitchStage) == 1);
static_assert(offsetof(PitchStage, pitch) == 0 && offsetof(PitchStage, fft) == 4 &&
              offsetof(PitchStage, reserved) == 6 && offsetof(PitchStage, instance) == 8);
static_assert(sizeof(SongLinkPacket) == 1436);
static_assert(sizeof(PitchPacket) == 32 + 8 + 64 * 12);
static_assert(sizeof(EffectsPacket) == 4132);
static_assert(sizeof(EffectsPacketV2) == 4140);
inline constexpr int SONG_LINK_PORT = 39022;
inline int receiverPort() {
#ifdef OBS_JUKEBOX_QA
    static const int configured = [] {
        auto value = std::getenv("OBS_JUKEBOX_TEST_PORT");
        if (!value)
            return SONG_LINK_PORT;
        char *end = nullptr;
        auto port = std::strtol(value, &end, 10);
        return end != value && *end == '\0' && port >= 1024 && port <= 65535 ? int(port) : SONG_LINK_PORT;
    }();
    return configured;
#else
    return SONG_LINK_PORT;
#endif
}
