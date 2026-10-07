#pragma once
#include <string>
#include "LinkPacket.hpp"
#include "Socket.hpp"
namespace separate_song {
struct Snapshot {
    bool musicPosition = false;
    bool playing = false, enabled = true;
    double position = 0, rate = 1, offset = 0;
    uint64_t timestamp = 0;
    float musicVolume = 1.f, effectsVolume = 1.f;
    unsigned epoch = 0;
    int attempt = 0;
    std::string status = "Ready", level, song, path;
};
class Bridge {
    SongSocket socket = BAD_SOCKET;
public:
    ~Bridge();
    bool start();
    void publish(const Snapshot& snapshot);
};
Bridge& bridge();
}
