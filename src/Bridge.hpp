#pragma once
#include <string>
#include <array>
#include <atomic>
#include <thread>
#include "LinkPacket.hpp"
#include "Socket.hpp"
namespace separate_song {
struct Snapshot {
    bool musicPosition = false;
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
};
class Bridge {
    SongSocket socket = BAD_SOCKET;
    std::atomic<bool> stop{false};
    std::thread worker;
    bool longPathLogged = false;

  public:
    ~Bridge();
    bool start();
    void shutdown();
    void publish(const Snapshot &snapshot);
};
Bridge &bridge();
} // namespace separate_song
