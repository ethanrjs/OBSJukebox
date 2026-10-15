#include "Bridge.hpp"
#include "LinkKey.hpp"
#include "MonotonicClock.hpp"
#include "Utf8.hpp"
#include <cstring>
#include <algorithm>
#if __has_include(<Geode/Geode.hpp>)
#include <Geode/Geode.hpp>
#else
#include <cstdio>
#endif
namespace separate_song {
namespace {
// Copies at most size-1 bytes without splitting a UTF-8 sequence; the rest of out stays zeroed.
template <size_t N> void copyText(char (&out)[N], const std::string &text) {
    size_t length = std::min(text.size(), N - 1);
    if (length < text.size())
        while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
            --length;
    std::memcpy(out, text.data(), length);
}
} // namespace
// Lets at most 32 datagrams through per 50 ms, so a flood can't keep the clock thread spinning.
struct DatagramBudget {
    unsigned used = 0;
    uint64_t window = 0;
    bool spend(uint64_t now) {
        if (now - window >= 50000000) {
            window = now;
            used = 0;
        }
        return used++ < 32;
    }
};
bool answersProbe(const ClockSyncPacket &p, long size, const sockaddr_in &peer) {
    return size == sizeof(p) && !std::memcmp(p.magic, "GDCLK1\0", 8) && p.version == 2 && p.kind == 1 &&
           p.sessionID == processSessionID() && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
           signedWith(processLinkKey(), p);
}
Bridge::~Bridge() { shutdown(); }
void Bridge::shutdown() {
    stop = true;
    if (worker.joinable())
        worker.join();
    if (socket != BAD_SOCKET)
        closeSocket(socket);
    socket = BAD_SOCKET;
}
bool Bridge::start() {
    if (socket != BAD_SOCKET)
        return true;
    socket = openLoopbackSocket();
    if (socket == BAD_SOCKET)
        return false;
    if (!writeLinkKey(processLinkKey())) {
#if __has_include(<Geode/Geode.hpp>)
        geode::log::error("OBS Jukebox could not write its link key, so OBS will ignore the game");
#else
        std::fprintf(stderr, "OBS Jukebox could not write its link key, so OBS will ignore the game\n");
#endif
    }
    stop = false;
    worker = std::thread([this] {
        DatagramBudget budget;
        while (!stop) {
            if (!waitSocketReadable(socket, 50))
                continue;
            // Over budget, the rest wait in the socket buffer, and the system drops what doesn't fit.
            if (!budget.spend(monotonicNowNs())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            ClockSyncPacket p{};
            sockaddr_in peer{};
#ifdef _WIN32
            int length = sizeof(peer);
#else
            socklen_t length = sizeof(peer);
#endif
            auto size = recvfrom(socket, reinterpret_cast<char *>(&p), sizeof(p), 0,
                                 reinterpret_cast<sockaddr *>(&peer), &length);
            auto received = monotonicNowNs();
            if (answersProbe(p, long(size), peer)) {
                p.kind = 2;
                p.t2 = received;
                p.t3 = monotonicNowNs();
                signPacket(processLinkKey(), p);
                sendto(socket, reinterpret_cast<const char *>(&p), sizeof(p), 0,
                       reinterpret_cast<sockaddr *>(&peer), length);
            }
        }
    });
    return true;
}
void Bridge::publish(const Snapshot &s) {
    if (socket == BAD_SOCKET)
        return;
    SongLinkPacket p;
    p.flags = (s.enabled ? 1 : 0) | (s.playing ? 2 : 0) | (s.looping ? 4 : 0);
    p.epoch = s.epoch;
    p.attempt = s.attempt;
    p.position = s.position;
    p.rate = s.rate;
    p.offset = s.offset;
    p.musicVolume = s.musicVolume;
    p.effectsVolume = s.effectsVolume;
    p.timestamp = s.timestamp ? s.timestamp : monotonicNowNs();
    p.sessionID = processSessionID();
    p.loopStart = s.loopStart;
    p.loopEnd = s.loopEnd;
    p.channelID = s.channelID;
    p.triggerGain = s.triggerGain;
    p.fadeCount = std::min(s.fadeCount, 8u);
    std::copy_n(s.fades.begin(), p.fadeCount, p.fades);
    copyText(p.status, s.status);
    copyText(p.level, s.level);
    copyText(p.song, s.song);
    // Paths are never shortened: a cut path names the wrong file, so a long one stops playback instead.
    // A path that isn't UTF-8 stops playback too: OBS can't open it.
    bool tooLong = s.path.size() >= sizeof(p.path);
    if (!tooLong && validUtf8(s.path)) {
        std::strncpy(p.path, s.path.c_str(), sizeof(p.path) - 1);
    } else {
        p.flags &= ~2u;
        std::memset(p.status, 0, sizeof(p.status));
        copyText(p.status, std::string(tooLong ? "Song path too long" : "Song path is not UTF-8"));
        if (tooLong && !longPathLogged) {
#if __has_include(<Geode/Geode.hpp>)
            geode::log::warn("OBS song path exceeds the {} byte transport limit ({} bytes)",
                             sizeof(p.path) - 1, s.path.size());
#else
            std::fprintf(stderr, "OBS song path exceeds the %zu byte transport limit (%zu bytes)\n",
                         sizeof(p.path) - 1, s.path.size());
#endif
            longPathLogged = true;
        }
#if __has_include(<Geode/Geode.hpp>)
        if (!tooLong && !invalidPathLogged) {
            geode::log::warn("OBS song path is not valid UTF-8, so OBS can't play it");
            invalidPathLogged = true;
        }
#endif
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(receiverPort());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    signPacket(processLinkKey(), p);
    sendto(socket, reinterpret_cast<const char *>(&p), sizeof(p), 0, reinterpret_cast<sockaddr *>(&address),
           sizeof(address));
}
// Shutdown is explicit before FMOD/application teardown, never under DLL loader lock.
Bridge &bridge() {
    static auto *b = new Bridge;
    return *b;
}
} // namespace separate_song
