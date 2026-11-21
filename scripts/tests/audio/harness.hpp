#pragma once
#include "../../../obs-plugin/separate-song.cpp"
#include <iostream>
#include <fstream>
#include <limits>
#include <random>
#include "../../../src/MonotonicClock.hpp"
#include "../../../src/Bridge.hpp"
#include "../../../src/TapClock.hpp"
#include "../../../src/Utf8.hpp"
#ifdef _WIN32
    #include <timeapi.h>
#endif

static SongLinkPacket published;

static int captureSend(
    SongSocket, char const* data, int size, int, sockaddr const*,
#ifdef _WIN32
    int
#else
    socklen_t
#endif
) {
    if (size == sizeof(published)) std::memcpy(&published, data, size);
    return size;
}

#define sendto captureSend
#include "../../../src/Bridge.cpp"
#undef sendto

static int failures = 0;

static void check(bool ok, char const* message) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << message << '\n';
    if (!ok) ++failures;
}

static void writeRamp(char const* path, bool negative = false) {
    std::ofstream out(path, std::ios::binary);
    auto word = [&](uint32_t n, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out.put(char(n >> (i * 8)));
    };
    constexpr uint32_t frames = 48000 * 4, bytes = frames * 4;
    out.write("RIFF", 4);
    word(36 + bytes, 4);
    out.write("WAVEfmt ", 8);
    word(16, 4);
    word(1, 2);
    word(2, 2);
    word(48000, 4);
    word(48000 * 4, 4);
    word(4, 2);
    word(16, 2);
    out.write("data", 4);
    word(bytes, 4);
    for (unsigned i = 0; i < frames; ++i) {
        auto sample = int16_t(32768 * (.1 + double(i) / 480000) * (negative ? -1 : 1));
        word(uint16_t(sample), 2);
        word(uint16_t(sample), 2);
    }
}

static void sendState(SongSocket socket, SongLinkPacket p, uint64_t capture) {
    p.timestamp = capture;
    signPacket(processLinkKey(), p);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(receiverPort());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto sent = sendto(
        socket,
        reinterpret_cast<char const*>(&p),
        int(sizeof(p)),
        0,
        reinterpret_cast<sockaddr*>(&addr),
        sizeof(addr)
    );
    if (sent != int(sizeof(p))) check(false, "loopback send succeeds");
}

static std::vector<Captured> capturedBlocks() {
    std::lock_guard lock(capturedMutex);
    return captured;
}

static void resetOutput() {
    std::lock_guard lock(capturedMutex);
    captured.clear();
}

// Waits for the game's next 10 ms song packet. A plain sleep_for(10 ms) takes 15.6 ms on
// Windows' default timer ticks, so this uses the waitable timer of the output thread.
static void sleepTick() {
    static thread_local BlockTimer timer;
    static thread_local Clock::time_point next{};
    auto const now = Clock::now();
    if (next < now) next = now;
    next += std::chrono::milliseconds(10);
    timer.sleepUntil(next);
}

// A receiver and a source on simulated time: no sockets, threads or sleeps, and packets arrive at
// once. Clock::now() stays real, so the retry, recovery and connect tests do not follow it.
struct Simulation {
    // A game whose clock is `offset` ns from OBS's. Songs and effects come from separate ports. It
    // signs with the key in the test's key file unless a test gives it another.
    struct Game {
        uint64_t session;
        int64_t offset;
        uint16_t port, effectsPort;
        LinkKey key = processLinkKey();

        uint64_t stamp(uint64_t obs) const {
            return uint64_t(int64_t(obs) + offset);
        }
    };

    uint64_t now = 5000000000000ULL; // 5000 s, so a game clock an hour behind stays positive
    uint64_t nextPoll = now + SongSource::blockNs, next = 0;
    bool started = false;
    std::unique_ptr<SongSource> source;
    std::vector<Captured> output;
    Game local; // sends when a test names no game; on OBS's clock, synced by its first packet
    Game const* answering = nullptr;

    Simulation() {
        receiver.reset(new Receiver(Receiver::Offline{}));
        source = std::make_unique<SongSource>(nullptr);
        source->active = true;
        local = game();
        answering = &local;
    }

    ~Simulation() {
        source.reset();
        receiver.reset();
    }

    uint64_t sessions = 0;

    Game game(int64_t offset = 0, uint16_t port = 50001) {
        return {++sessions, offset, port, uint16_t(port + 1)};
    }

    std::deque<uint64_t> const& probes() const {
        return receiver->probes;
    }

    void reply(Game const& game, uint64_t t1, uint64_t senderTime) {
        ClockSyncPacket p;
        p.kind = 2;
        p.sessionID = game.session;
        p.t1 = t1;
        p.t2 = p.t3 = senderTime;
        signPacket(game.key, p);
        handle(&p, sizeof(p), game.port);
    }

    void answerProbes() {
        if (!answering) return;
        auto sent = receiver->probes;
        for (auto t1 : sent)
            reply(*answering, t1, answering->stamp(t1));
    }

    void deliver(void const* data, size_t size, uint16_t port) {
        receiver->probe(now);
        answerProbes();
        handle(data, size, port);
        answerProbes();
    }

    // A song packet of any version. `key` signs it, but versions 3 to 6 end before the tag.
    void sendRaw(
        SongLinkPacket p, uint64_t capture, int version, uint16_t port, LinkKey const* key = nullptr
    ) {
        p.version = version;
        p.timestamp = capture;
        if (key) signPacket(*key, p);
        deliver(
            &p,
            version == 3     ? SONG_LINK_V3_SIZE :
                version == 4 ? SONG_LINK_V4_SIZE :
                version == 5 ? SONG_LINK_V5_SIZE :
                version == 6 ? SONG_LINK_V6_SIZE :
                               sizeof(p),
            port
        );
    }

    void send(Game const& game, SongLinkPacket p, uint64_t capture) {
        p.sessionID = game.session;
        sendRaw(p, game.stamp(capture), 7, game.port, &game.key);
    }

    void send(SongLinkPacket p, uint64_t capture) {
        send(local, p, capture);
    }

    void sendEffects(Game const& game, EffectsPacketV3 e, uint16_t port) {
        auto size = offsetof(EffectsPacketV3, samples) + e.frames * 8;
        signPacket(game.key, e, size);
        deliver(&e, size, port);
    }

    void sendEffects(EffectsPacketV3 const& e, uint16_t port) {
        sendEffects(local, e, port);
    }

    void sendEffects(EffectsPacketV3 e) {
        e.sessionID = local.session;
        sendEffects(e, local.effectsPort);
    }

    void sendEffects(EffectsPacket const& e, uint16_t port) {
        deliver(&e, 36 + e.frames * 8, port);
    }

    // Says hello every 10 ms and answers clock probes, as SyncedSender does, until the receiver
    // follows this session within 100 ms of OBS's clock. It keeps answering afterwards.
    bool synchronize(Game const& game, unsigned epoch = 717) {
        answering = &game;
        SongLinkPacket hello;
        hello.epoch = epoch;
        hello.flags = 0;
        for (int i = 0; i < 60; ++i) {
            send(game, hello, now);
            advance(10000000);
            auto current = receiver->get();
            if (current.connected && current.packet.sessionID == game.session &&
                current.packet.epoch == epoch &&
                std::abs(double(int64_t(current.packet.timestamp) - int64_t(now))) < 100000000)
                return true;
        }
        return false;
    }

    void start() {
        started = true;
        next = now - SongSource::bufferingNs + SongSource::blockNs;
    }

    void advance(uint64_t ns) {
        auto const until = now + ns;
        for (;;) {
            auto render = started ? next + SongSource::bufferingNs : UINT64_MAX;
            auto due = std::min(render, nextPoll);
            if (due > until) break;
            now = due;
            if (due == nextPoll) {
                receiver->probe(now);
                answerProbes();
                nextPoll += SongSource::blockNs;
            }
            if (due == render) renderNext();
        }
        now = until;
    }

private:
    void handle(void const* data, size_t size, uint16_t port) {
        sockaddr_in from{};
        from.sin_family = AF_INET;
        from.sin_port = htons(port);
        from.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        receiver->handle(static_cast<unsigned char const*>(data), size, from, now);
    }

    void renderNext() {
        AudioBlock music{}, effects{};
        source->renderMusic(next, music, now);
        if (source->active) source->renderEffects(next, effects);
        auto const generation = source->outputGeneration.load();
        source->musicBlocks.push(next, music, generation, next);
        source->effectsBlocks.push(next, effects, generation, next);
        auto audio = source->mixBlock(next);
        output.push_back({next, now, {audio.begin(), audio.end()}});
        next += SongSource::blockNs;
    }
};

// A one-second stereo IEEE float WAV whose every sample is `value`.
static void writeFloatWav(char const* path, float value) {
    std::ofstream out(path, std::ios::binary);
    auto word = [&](uint32_t n, int bytes) {
        for (int i = 0; i < bytes; ++i)
            out.put(char(n >> (i * 8)));
    };
    constexpr uint32_t frames = 48000, bytes = frames * 8;
    out.write("RIFF", 4);
    word(36 + bytes, 4);
    out.write("WAVEfmt ", 8);
    word(16, 4);
    word(3, 2);
    word(2, 2);
    word(48000, 4);
    word(48000 * 8, 4);
    word(8, 2);
    word(32, 2);
    out.write("data", 4);
    word(bytes, 4);
    for (uint32_t i = 0; i < frames * 2; ++i)
        out.write(reinterpret_cast<char const*>(&value), 4);
}

// A four-second tone at `hz`, so a render test can tell frequencies apart by counting crossings.
static void writeTone(const char *path, double hz = 440) {
    std::ofstream out(path, std::ios::binary);
    constexpr uint32_t frames = 48000 * 4, bytes = frames * 4;
    auto word = [&](uint32_t value, int count) {
        for (int i = 0; i < count; ++i) out.put(char(value >> (i * 8)));
    };
    out.write("RIFF", 4); word(36 + bytes, 4); out.write("WAVEfmt ", 8); word(16, 4);
    word(1, 2); word(2, 2); word(48000, 4); word(48000 * 4, 4); word(4, 2); word(16, 2);
    out.write("data", 4); word(bytes, 4);
    for (uint32_t i = 0; i < frames; ++i) {
        auto sample = int16_t(32767 * .3 * std::sin(2 * 3.14159265358979 * hz * double(i) / 48000));
        word(uint16_t(sample), 2); word(uint16_t(sample), 2);
    }
}

struct SyncedSender {
    SongSocket socket = BAD_SOCKET;
    std::atomic<int64_t> clockOffset;
    uint64_t session;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> probes{0};
    std::thread replies;

    explicit SyncedSender(int64_t offset) : clockOffset(offset), session(os_gettime_ns()) {
        socket = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(
            socket != BAD_SOCKET &&
                bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0,
            "foreign-clock sender binds loopback"
        );
        nonblocking(socket);
        replies = std::thread([this] {
            while (!stop) {
                ClockSyncPacket probe{};
                sockaddr_in peer{};
#ifdef _WIN32
                int peerSize = sizeof(peer);
#else
                socklen_t peerSize = sizeof(peer);
#endif
                auto n = recvfrom(
                    socket,
                    reinterpret_cast<char*>(&probe),
                    sizeof(probe),
                    0,
                    reinterpret_cast<sockaddr*>(&peer),
                    &peerSize
                );
                auto received = stamp(os_gettime_ns());
                if (n == sizeof(probe) && !std::memcmp(probe.magic, "GDCLK1\0", 8) &&
                    probe.version == 2 && probe.kind == 1 && probe.sessionID == session &&
                    signedWith(processLinkKey(), probe)) {
                    ++probes;
                    probe.kind = 2;
                    probe.t2 = received;
                    probe.t3 = stamp(os_gettime_ns());
                    signPacket(processLinkKey(), probe);
                    sendto(
                        socket,
                        reinterpret_cast<char const*>(&probe),
                        sizeof(probe),
                        0,
                        reinterpret_cast<sockaddr*>(&peer),
                        peerSize
                    );
                }
                else {
                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(socket, &readable);
                    timeval timeout{};
                    timeout.tv_usec = 10000;
                    select(int(socket) + 1, &readable, nullptr, nullptr, &timeout);
                }
            }
        });
    }

    ~SyncedSender() {
        stop = true;
        if (replies.joinable()) replies.join();
        closeSocket(socket);
    }

    uint64_t stamp(uint64_t native) const {
        return uint64_t(int64_t(native) + clockOffset.load());
    }

    void send(SongLinkPacket p, uint64_t native) {
        p.sessionID = session;
        sendState(socket, p, stamp(native));
    }

    bool synchronize(unsigned epoch = 717) {
        SongLinkPacket hello;
        hello.epoch = epoch;
        hello.flags = 0;
        for (int i = 0; i < 60; ++i) {
            send(hello, os_gettime_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto current = receiver->get();
            if (current.connected && current.packet.sessionID == session &&
                current.packet.epoch == epoch &&
                std::abs(double(int64_t(current.packet.timestamp) - int64_t(os_gettime_ns()))) <
                    100000000)
                return true;
        }
        auto current = receiver->get();
        std::cerr << "Clock handshake failed: probes=" << probes
                  << ", connected=" << current.connected << ", session=" << current.packet.sessionID
                  << " expected=" << session
                  << ", stampDelta=" << int64_t(current.packet.timestamp) - int64_t(os_gettime_ns())
                  << '\n';
        return false;
    }
};
