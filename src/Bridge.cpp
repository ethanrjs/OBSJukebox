#include "Bridge.hpp"
#include <cstring>
namespace separate_song {
Bridge::~Bridge() { if (socket != BAD_SOCKET) closeSocket(socket); }
bool Bridge::start() {
    if(!socketsReady())return false;
    socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket == BAD_SOCKET) return false;
    nonblocking(socket);
    return true;
}
void Bridge::publish(const Snapshot& s) {
    if (socket == BAD_SOCKET) return;
    SongLinkPacket p;
    p.flags = (s.enabled ? 1 : 0) | (s.playing ? 2 : 0);
    p.epoch = s.epoch; p.attempt = s.attempt;
    p.position = s.position; p.rate = s.rate; p.offset = s.offset;
    p.musicVolume = s.musicVolume; p.effectsVolume = s.effectsVolume;
    std::strncpy(p.status, s.status.c_str(), sizeof(p.status) - 1);
    std::strncpy(p.level, s.level.c_str(), sizeof(p.level) - 1);
    std::strncpy(p.song, s.song.c_str(), sizeof(p.song) - 1);
    if (s.path.size() < sizeof(p.path))
        std::strncpy(p.path, s.path.c_str(), sizeof(p.path) - 1);
    else p.flags &= ~2u;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(SONG_LINK_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(socket, reinterpret_cast<const char*>(&p), sizeof(p), 0, reinterpret_cast<sockaddr*>(&address), sizeof(address));
}
Bridge& bridge() { static Bridge b; return b; }
}
