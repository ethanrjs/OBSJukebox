#pragma once
#include <string>
#include <array>
#include <atomic>
#include <thread>
#include <algorithm>
#include "LinkPacket.hpp"
#include "Socket.hpp"
namespace separate_song {
struct Snapshot {
    bool playing = false, enabled = true;
    double position = 0, rate = 1, offset = 0;
    float musicVolume = 1.f, effectsVolume = 1.f;
    unsigned epoch = 0;
    int attempt = 0;
    std::string status = "Ready", level, song, path;
    uint64_t timestamp = 0;
    int32_t channelID = 0;
    float triggerGain = 1.f;
    uint32_t fadeCount = 0;
    std::array<SongFadePoint, 8> fades{};
    bool looping = false;
    double loopStart = 0, loopEnd = 0;
    std::array<PitchStage, 8> stages{}, effectsStages{};
    uint8_t stageCount = 0, effectsStageCount = 0;
    bool groupExcluded = false;
    // A stop must not predate the last mixer-dated sample. Keep its stages and exclusion so OBS
    // fades the voice through the same processing, even after the game's channel disappears.
    void markStopped(uint64_t now, unsigned newEpoch) {
        timestamp = std::max(timestamp, now);
        playing = false;
        epoch = newEpoch;
    }
};
class Bridge {
    SongSocket socket = BAD_SOCKET;
    std::atomic<bool> stop{false};
    std::thread worker;
    bool longPathLogged = false, invalidPathLogged = false;
    void send(const void *packet, size_t size);

  public:
    ~Bridge();
    bool start();
    void shutdown();
    void publish(const Snapshot &snapshot);
};
Bridge &bridge();
// Whether a datagram is a clock probe the game should answer.
bool answersProbe(const ClockSyncPacket &probe, long size, const sockaddr_in &peer);
} // namespace separate_song
