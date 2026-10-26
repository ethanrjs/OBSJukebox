#include "../src/LinkPacket.hpp"
#include "../src/Socket.hpp"
#include "../src/Utf8.hpp"
#include "Decoder.hpp"
#include "LinkClock.hpp"
#include "PitchShifter.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <obs-module.h>
#include <string>
#include <thread>
#include <util/platform.h>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#if defined(__linux__)
#include "LinuxPaths.hpp"
#endif

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) {
    return "OBS Jukebox: Geometry Dash custom song and sound effects";
}
MODULE_EXPORT const char *obs_module_name(void) { return "OBS Jukebox"; }
MODULE_EXPORT const char *obs_module_author(void) { return "babbur"; }

// The release version from mod.json, set by the build. The fallback covers builds that don't set it,
// such as a bare editor compile.
#ifndef OBS_JUKEBOX_VERSION
#define OBS_JUKEBOX_VERSION "dev"
#endif

using Clock = std::chrono::steady_clock;
static void configureAudioThread() {
#ifdef __APPLE__
    // Both workers must meet 10 ms audio deadlines, including in background OBS.
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0)
        blog(LOG_ERROR, "OBS Jukebox: could not configure audio thread scheduling");
#endif
}
struct LinkState {
    SongLinkPacket packet;
    Clock::time_point received{};
    bool connected = false;
};
static bool knownFft(uint16_t fft) {
    return fft == 256 || fft == 512 || fft == 1024 || fft == 2048 || fft == 4096;
}
// A stage the mod observed on the way to the speakers, as FMOD reads its parameters back.
static bool validStage(const PitchStage &stage) {
    return std::isfinite(stage.pitch) && stage.pitch >= .5f && stage.pitch <= 2.f && knownFft(stage.fft) &&
           stage.reserved == 0 && stage.instance != 0;
}
static bool emptyStage(const PitchStage &stage) {
    static const PitchStage zero;
    return !memcmp(&stage, &zero, sizeof(stage));
}
static bool validStages(const PitchStage *stages, uint8_t count) {
    for (unsigned i = 0; i < 8; ++i)
        if (i < count ? !validStage(stages[i]) : !emptyStage(stages[i]))
            return false;
    return true;
}
// Checks a v6 song packet's size and fields. Returns nothing if any of them is invalid.
static std::optional<SongLinkPacket> parseSongPacket(const unsigned char *data, size_t size) {
    SongLinkPacket p{};
    if (size != sizeof(p))
        return std::nullopt;
    memcpy(&p, data, size);
    if (p.version != 6 || memcmp(p.magic, "GDSONG1", 8) || !p.sessionID)
        return std::nullopt;
    if (!std::isfinite(p.musicVolume) || p.musicVolume < 0 || p.musicVolume > 1 ||
        !std::isfinite(p.effectsVolume) || p.effectsVolume < 0 || p.effectsVolume > 1 ||
        !std::isfinite(p.position) || p.position < -600 || p.position >= 86400 ||
        !std::isfinite(p.offset) || !std::isfinite(p.rate) || p.rate <= 0 || p.rate > 16 ||
        p.channelID < -1 || p.channelID >= SONG_LINK_MAX_CHANNELS ||
        !std::isfinite(p.triggerGain) || p.triggerGain < 0 || p.triggerGain > 16 ||
        p.fadeCount > 8 || (p.flags & ~15u) || p.stageCount > 8 || p.effectsStageCount > 8 ||
        p.stageReserved[0] || p.stageReserved[1])
        return std::nullopt;
    if (p.channelID < 0 ? (p.flags & 8) != 0 : p.effectsStageCount != 0)
        return std::nullopt;
    if ((p.flags & 8) && p.stageCount)
        return std::nullopt;
    if (!validStages(p.stages, p.stageCount) || !validStages(p.effectsStages, p.effectsStageCount))
        return std::nullopt;
    uint64_t previous = 0;
    for (unsigned i = 0; i < p.fadeCount; ++i) {
        const auto &f = p.fades[i];
        if (f.offsetNs < previous || f.offsetNs > 86400000000000ULL || !std::isfinite(f.gain) || f.gain < 0 ||
            f.gain > 16)
            return std::nullopt;
        previous = f.offsetNs;
    }
    if ((p.flags & 4) && (!std::isfinite(p.loopStart) || !std::isfinite(p.loopEnd) || p.loopStart < 0 ||
                          p.loopEnd <= p.loopStart || p.loopEnd > 86400))
        return std::nullopt;
    p.status[23] = p.level[95] = p.song[95] = p.path[1023] = 0;
    // The mod only sends UTF-8. Anything else would throw when the decoder builds a path from it.
    if (!separate_song::validUtf8(p.path))
        return std::nullopt;
    return p;
}
struct EffectsInput {
    EffectsPacket packet;
    uint64_t session = 0;
};
// Checks a GDSFX2 effects packet and converts it to the type the receiver stores.
static std::optional<EffectsInput> parseEffectsPacket(const unsigned char *data, size_t size) {
    constexpr size_t header = offsetof(EffectsPacketV2, samples);
    if (size < header || size > sizeof(EffectsPacketV2) || memcmp(data, "GDSFX2\0", 8))
        return std::nullopt;
    EffectsPacketV2 p;
    memcpy(&p, data, size);
    if (p.version != 2 || p.frames > 512 || size != header + p.frames * 2 * sizeof(float))
        return std::nullopt;
    EffectsInput input;
    auto &effect = input.packet;
    effect.sequence = p.sequence;
    effect.sampleRate = p.sampleRate;
    effect.frames = p.frames;
    effect.timestamp = p.timestamp;
    effect.stream = p.stream;
    input.session = p.sessionID;
    std::copy_n(p.samples, p.frames * 2, effect.samples);
    if (effect.frames < 1 || effect.sampleRate < 8000 || effect.sampleRate > 192000 || effect.stream > 1)
        return std::nullopt;
#ifndef OBS_JUKEBOX_QA
    if (effect.stream != 0)
        return std::nullopt;
#endif
    for (unsigned i = 0; i < effect.frames * 2; ++i)
        if (!std::isfinite(effect.samples[i]))
            return std::nullopt;
    return input;
}
// A song packet below version 6, or any GDSFX1 packet, comes from a mod older than this plugin: v1.0
// sent v3 song packets and GDSFX1 effects, and v1.1 to v1.2 sent v5. handle() has checked size >= 8.
static bool fromOlderMod(const unsigned char *data, size_t size) {
    if (!memcmp(data, "GDSFX1\0", 8))
        return true;
    uint32_t version = 0;
    if (size < offsetof(SongLinkPacket, version) + sizeof(version) || memcmp(data, "GDSONG1", 8))
        return false;
    memcpy(&version, data + offsetof(SongLinkPacket, version), sizeof(version));
    return version < 6;
}
// A song packet above version 6, a GDSFX2 packet above version 2, or a later GDSONG or GDSFX
// magic comes from a mod newer than this plugin. handle() has checked that size >= 8.
static bool fromNewerMod(const unsigned char *data, size_t size) {
    uint32_t version = 0;
    if (!memcmp(data, "GDSONG1", 8)) {
        if (size < offsetof(SongLinkPacket, version) + sizeof(version))
            return false;
        memcpy(&version, data + offsetof(SongLinkPacket, version), sizeof(version));
        return version > 6;
    }
    if (!memcmp(data, "GDSFX2\0", 8)) {
        if (size < offsetof(EffectsPacketV2, version) + sizeof(version))
            return false;
        memcpy(&version, data + offsetof(EffectsPacketV2, version), sizeof(version));
        return version > 2;
    }
    return (!memcmp(data, "GDSONG", 6) && data[6] > '1' && data[6] <= '9') ||
           (!memcmp(data, "GDSFX", 5) && data[5] > '2' && data[5] <= '9');
}
// Whether an effect is within a second of now.
static bool effectsCurrent(uint64_t timestamp, uint64_t now) {
    return timestamp <= now + 1000000000 && timestamp + 1000000000 >= now;
}
// Time from t to now, or 0 if t is later. Callers read now before taking the lock that guards t, so
// the receiver thread may have stored a later t in between.
static uint64_t since(uint64_t t, uint64_t now) { return now > t ? now - t : 0; }
// OBS takes audio in 10 ms blocks of 480 stereo frames at 48 kHz.
constexpr size_t blockFrames = 480;
constexpr uint32_t blockRate = 48000;
using AudioBlock = std::array<float, blockFrames * 2>;

class Receiver {
    SongSocket sock = BAD_SOCKET;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    LinkState state;
    struct PlaybackPacket {
        SongLinkPacket packet;
        uint64_t senderTimestamp;
    };
    std::array<std::deque<PlaybackPacket>, SONG_LINK_MAX_CHANNELS + 1> playback;
    std::deque<EffectsPacket> effects, pendingEffects;
    LinkClock clock;
    uint64_t session = 0, lastProbe = 0, lastSongReceived = 0, lastOldModPacket = 0, lastNewModPacket = 0;
    std::array<uint64_t, 2> lastEffectsReceived{};
    std::array<uint16_t, 2> effectsPort{};
    static constexpr uint64_t sessionTimeoutNs = 2000000000ULL;
    std::deque<uint64_t> probes;
    sockaddr_in peer{};
    std::deque<SongLinkPacket> pending;
    FILE *reference = nullptr;
    void queueEffects(const EffectsPacket &effect) {
        auto next = std::lower_bound(effects.begin(), effects.end(), effect.timestamp,
                                     [](const auto &a, uint64_t t) { return a.timestamp < t; });
        if (next == effects.end() || next->timestamp != effect.timestamp)
            effects.insert(next, effect);
        while (effects.size() > 128)
            effects.pop_front();
    }
    void clearTimeline() {
        for (auto &voice : playback)
            voice.clear();
        effects.clear();
        state.connected = false;
    }
    // Forgets everything learned from the previous sender: its clock, waiting packets, effects
    // ports and timeline.
    void resetSession(uint64_t id) {
        session = id;
        clock.reset();
        pending.clear();
        pendingEffects.clear();
        probes.clear();
        lastProbe = 0;
        effectsPort.fill(0);
        lastEffectsReceived.fill(0);
        clearTimeline();
    }
    // The effects tap uses its own socket. Pin its port independently and permit a replacement
    // tap only after the previous one is stale.
    bool pinEffectsPort(unsigned stream, uint16_t port, uint64_t now) {
        if (effectsPort[stream] && effectsPort[stream] != port && now - lastEffectsReceived[stream] <= sessionTimeoutNs)
            return false;
        effectsPort[stream] = port;
        lastEffectsReceived[stream] = now;
        return true;
    }
    void accept(SongLinkPacket p, uint64_t now) {
        auto senderTimestamp = p.timestamp;
        auto &voice = playback[p.channelID + 1];
        auto existing = std::find_if(voice.begin(), voice.end(), [&](const auto &previous) {
            return previous.senderTimestamp == senderTimestamp;
        });
        // Keep a sampled state's timeline position stable even if its pause/stop update arrives
        // after a clock correction. Translated timestamps alone cannot identify the same sample.
        if (existing != voice.end())
            p.timestamp = existing->packet.timestamp;
        else if (!clock.translate(p.timestamp))
            return;
        if (p.timestamp > now + 100000000 || p.timestamp + 1000000000 < now)
            return;
        auto next = std::lower_bound(voice.begin(), voice.end(), p.timestamp,
                                     [](const auto &a, uint64_t t) { return a.packet.timestamp < t; });
        // FMOD's sampled position time can repeat within a mixer block. A later packet at
        // that time may pause/stop the voice or change its gain; it is not necessarily a duplicate.
        if (next != voice.end() && next->packet.timestamp == p.timestamp)
            *next = {p, senderTimestamp};
        else
            voice.insert(next, {p, senderTimestamp});
        while (voice.size() > 256)
            voice.pop_front();
        if (!state.connected || p.timestamp >= state.packet.timestamp)
            state = {p, Clock::now(), true};
    }
    void probe(uint64_t now) {
        if (!session || now - lastProbe < 250000000)
            return;
        ClockSyncPacket p;
        p.sessionID = session;
        p.t1 = now;
        probes.push_back(now);
        while (probes.size() > 8)
            probes.pop_front();
        lastProbe = now;
        sendto(sock, reinterpret_cast<const char *>(&p), sizeof(p), 0, reinterpret_cast<sockaddr *>(&peer),
               sizeof(peer));
    }
    // Handles one datagram from the game, received at `now`.
    void handle(const unsigned char *data, size_t size, const sockaddr_in &from, uint64_t now) {
        if (size < 8)
            return;
        if (!memcmp(data, "GDCLK1\0", 8))
            handleClockReply(data, size, from, now);
        else if (fromOlderMod(data, size)) {
            std::lock_guard lock(mutex);
            if (!lastOldModPacket)
                blog(LOG_WARNING, "[OBS Jukebox] Ignoring packets from an outdated OBS Jukebox mod; update the mod in Geometry Dash");
            lastOldModPacket = now;
        } else if (fromNewerMod(data, size)) {
            std::lock_guard lock(mutex);
            if (!lastNewModPacket)
                blog(LOG_WARNING, "[OBS Jukebox] Ignoring packets from a newer OBS Jukebox mod; update the OBS plugin");
            lastNewModPacket = now;
        } else if (!memcmp(data, "GDSFX", 5)) {
            if (auto effect = parseEffectsPacket(data, size))
                handleEffects(*effect, from, now);
        } else if (auto song = parseSongPacket(data, size))
            handleSong(*song, from, now);
    }
    void handleClockReply(const unsigned char *data, size_t size, const sockaddr_in &from, uint64_t now) {
        if (size != sizeof(ClockSyncPacket))
            return;
        ClockSyncPacket p;
        memcpy(&p, data, sizeof(p));
        auto found = std::find(probes.begin(), probes.end(), p.t1);
        if (p.version != 1 || p.kind != 2 || p.sessionID != session || found == probes.end() ||
            from.sin_port != peer.sin_port)
            return;
        probes.erase(found);
        bool changed = false;
        std::lock_guard lock(mutex);
        if (!clock.observe(p, now, changed))
            return;
        if (changed)
            clearTimeline();
        for (auto &waiting : pending)
            accept(waiting, now);
        pending.clear();
        for (auto waiting : pendingEffects)
            if (clock.translate(waiting.timestamp) && effectsCurrent(waiting.timestamp, now))
                queueEffects(waiting);
        pendingEffects.clear();
    }
    void handleEffects(const EffectsInput &input, const sockaddr_in &from, uint64_t now) {
        auto effect = input.packet;
        std::lock_guard lock(mutex);
        if (!session || input.session != session || !pinEffectsPort(effect.stream, from.sin_port, now))
            return;
        if (!clock.valid()) {
            if (effect.stream == 0) {
                pendingEffects.push_back(effect);
                while (pendingEffects.size() > 128)
                    pendingEffects.pop_front();
            }
            return;
        }
        if (!clock.translate(effect.timestamp) || !effectsCurrent(effect.timestamp, now))
            return;
        if (effect.stream == 1) {
            if (reference) {
                std::fwrite(&effect, sizeof(effect), 1, reference);
                std::fflush(reference);
            }
            return;
        }
        queueEffects(effect);
    }
    void handleSong(const SongLinkPacket &p, const sockaddr_in &from, uint64_t now) {
        std::lock_guard lock(mutex);
        if (session != p.sessionID || from.sin_port != peer.sin_port) {
            if (lastSongReceived && now - lastSongReceived <= sessionTimeoutNs)
                return;
            blog(LOG_INFO, "[OBS Jukebox] Adopting game session %llu", static_cast<unsigned long long>(p.sessionID));
            resetSession(p.sessionID);
            peer = from;
        }
        lastSongReceived = now;
        if (!clock.valid()) {
            pending.push_back(p);
            while (pending.size() > 256)
                pending.pop_front();
            probe(now);
            return;
        }
        accept(p, now);
    }

  public:
#ifdef OBS_JUKEBOX_QA
    // The audio tests run a receiver without its socket or thread and feed it packets on
    // simulated time.
    friend struct Simulation;
    struct Offline {};
    explicit Receiver(Offline) {}
#endif
    Receiver() {
        if (!socketsReady())
            return;
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(receiverPort()));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (sock == BAD_SOCKET || bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
            if (sock != BAD_SOCKET)
                closeSocket(sock);
            sock = BAD_SOCKET;
            blog(LOG_ERROR, "[OBS Jukebox] Could not bind local game link on port %d", receiverPort());
            return;
        }
        nonblocking(sock);
        int receiveBuffer = 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&receiveBuffer),
                   sizeof(receiveBuffer));
#ifdef OBS_JUKEBOX_QA
        if (auto path = std::getenv("SEPARATE_SONG_REFERENCE_FILE"))
            reference = std::fopen(path, "wb");
#endif
        worker = std::thread([this] {
            while (!stop) {
                std::array<unsigned char, sizeof(EffectsPacketV2) + 1> buffer{};
                sockaddr_in from{};
#ifdef _WIN32
                int length = sizeof(from);
#else
                socklen_t length = sizeof(from);
#endif
                auto size = recvfrom(sock, reinterpret_cast<char *>(buffer.data()), static_cast<int>(buffer.size()),
                                     0, reinterpret_cast<sockaddr *>(&from), &length);
                auto now = os_gettime_ns();
                probe(now);
                if (size <= 0) {
                    waitSocketReadable(sock, 50);
                    continue;
                }
                if (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK))
                    handle(buffer.data(), size_t(size), from, now);
            }
        });
    }
    ~Receiver() {
        stop = true;
        if (worker.joinable())
            worker.join();
        if (reference)
            std::fclose(reference);
        if (sock != BAD_SOCKET)
            closeSocket(sock);
    }
    bool available() const { return sock != BAD_SOCKET; }
    // Whether a mod older than v1.1 has sent packets within sessionTimeoutNs.
    bool oldModSending(uint64_t now) {
        std::lock_guard lock(mutex);
        return lastOldModPacket && since(lastOldModPacket, now) <= sessionTimeoutNs;
    }
    // Whether a mod newer than this plugin has sent packets within sessionTimeoutNs.
    bool newModSending(uint64_t now) {
        std::lock_guard lock(mutex);
        return lastNewModPacket && since(lastNewModPacket, now) <= sessionTimeoutNs;
    }
    // GD's main thread sends song packets and FMOD's mixer thread sends effects. Effects that keep
    // arriving after song packets stop mean the main thread is stalled (as while GD tears down a
    // level), and the game is still playing its music.
    bool mainThreadStalled(uint64_t now) {
        std::lock_guard lock(mutex);
        return lastSongReceived && lastEffectsReceived[0] && since(lastEffectsReceived[0], now) < 100000000 &&
               since(lastSongReceived, now) > 200000000;
    }
    LinkState get() {
        std::lock_guard lock(mutex);
        return state;
    }
    std::vector<int> getChannels() {
        std::lock_guard lock(mutex);
        std::vector<int> ids;
        for (int i = 0; i <= SONG_LINK_MAX_CHANNELS; ++i)
            if (!playback[i].empty())
                ids.push_back(i - 1);
        return ids;
    }
    std::vector<SongLinkPacket> playbackWindow(uint64_t start, uint64_t end, int channel = 0) {
        std::lock_guard lock(mutex);
        std::vector<SongLinkPacket> window(1);
        window.front().channelID = channel;
        if (channel < -1 || channel >= SONG_LINK_MAX_CHANNELS)
            return window;
        auto &voice = playback[channel + 1];
        auto next = std::upper_bound(voice.begin(), voice.end(), start,
                                     [](uint64_t t, const auto &p) { return t < p.packet.timestamp; });
        if (next != voice.begin())
            window.front() = std::prev(next)->packet;
        for (; next != voice.end() && next->packet.timestamp < end; ++next)
            window.push_back(next->packet);
        return window;
    }
    // A packet runs until the next packet in sequence starts, and its last sample blends into that
    // packet's first, so seams sound like any other pair of samples. The tap's timestamps can leave
    // a sub-sample gap or overlap between packets as it follows the wall clock; this absorbs it.
    void mixEffects(float *output, size_t frames, uint64_t start, float gain) {
        std::lock_guard lock(mutex);
        for (auto packet = effects.begin(); packet != effects.end(); ++packet) {
            const auto &p = *packet;
            auto after = std::next(packet);
            double duration = double(p.frames) / p.sampleRate;
            // Only join adjacent audio. Sequence numbers alone cannot distinguish a mixer stall
            // from uninterrupted capture. Two source samples cover tap slew, clock slew and rounding.
            const EffectsPacket *next =
                after != effects.end() && after->sampleRate == p.sampleRate &&
                        after->sequence == p.sequence + 1 && after->timestamp > p.timestamp &&
                        std::abs((double(after->timestamp) - double(p.timestamp)) / 1e9 - duration) <=
                            2.0 / p.sampleRate
                    ? &*after
                    : nullptr;
            double delta = (double(start) - double(p.timestamp)) / 1e9;
            double lastTime = double(p.frames - 1) / p.sampleRate;
            double length = next ? (double(next->timestamp) - double(p.timestamp)) / 1e9 : duration;
            if (delta >= length || delta + double(frames) / blockRate < 0)
                continue;
            for (size_t i = 0; i < frames; ++i) {
                double time = delta + double(i) / blockRate;
                if (time < 0 || time >= length)
                    continue;
                unsigned a = std::min(unsigned(time * p.sampleRate), p.frames - 1);
                bool seam = a + 1 == p.frames;
                double blend = seam ? (next ? (time - lastTime) / (length - lastTime) : 0)
                                    : time * p.sampleRate - a;
                for (unsigned ch = 0; ch < 2; ++ch) {
                    float from = p.samples[a * 2 + ch],
                          to = seam ? (next ? next->samples[ch] : from) : p.samples[(a + 1) * 2 + ch];
                    output[i * 2 + ch] += gain * float(double(from) + (double(to) - double(from)) * blend);
                }
            }
        }
    }
};
static std::shared_ptr<Receiver> receiver;

// The first frame of the block starting at `start` that is at or after `at`.
static size_t frameAt(uint64_t start, uint64_t at) {
    return at <= start ? size_t(0)
                       : size_t(std::min<uint64_t>(blockFrames, ((at - start) * blockRate + 999999999) / 1000000000));
}
static uint64_t frameTime(uint64_t start, size_t frame) { return start + frame * 1000000000 / blockRate; }
// Calls run(packet, first, end) for each run of frames in the block starting at `start` that one
// packet of a playbackWindow describes.
template <class Run> static void forEachRun(const std::vector<SongLinkPacket> &window, uint64_t start, Run run) {
    for (size_t i = 0; i < window.size(); ++i) {
        const auto &p = window[i];
        size_t first = i ? frameAt(start, p.timestamp) : 0;
        size_t end = i + 1 < window.size() ? frameAt(start, window[i + 1].timestamp) : blockFrames;
        if (p.timestamp && end > first)
            run(p, first, end);
    }
}
#ifdef OBS_JUKEBOX_QA
// A lock-free maximum for the output thread's QA counters; no allocation or logging there.
static void qaMax(std::atomic<uint64_t> &value, uint64_t next) {
    auto current = value.load(std::memory_order_relaxed);
    while (next > current && !value.compare_exchange_weak(current, next, std::memory_order_relaxed)) {
    }
}
#endif
// Applies one run's stages to a chain, starting fresh when the run belongs to a new process session.
// Only the thread that owns the chain calls this, so a session boundary never carries FIFO state.
static void processStages(PitchChain &chain, uint64_t &session, uint64_t id, float *audio, size_t frames,
                          const PitchStage *stages, uint8_t count) {
    if (session != id) {
        chain.reset();
        session = id;
    }
    chain.process(audio, frames, stages, count);
}

// Length of the crossfade at every music discontinuity (pause, resume, restart, seek, song
// change): 5 ms at 48 kHz. FMOD ramps these in game; without it OBS hears a click.
constexpr size_t declickFrames = 240;

struct SongSource {
    obs_source_t *source;
    std::atomic<bool> stop{false}, active{false};
    std::mutex mutex;
    static constexpr const char *noSongDetail = "Select the OBS checkbox beside a Jukebox song.";
    std::string detail = noSongDetail;
    std::thread worker, decoderWorker, effectsWorker;
    std::shared_ptr<Receiver> linkReceiver;
    static constexpr uint64_t blockNs = blockFrames * 1000000000ULL / blockRate, bufferingNs = 40000000,
                              staleNs = 400000000;
    // The output thread owns no decoder, filesystem object or pitch stage. Two workers render
    // timestamped blocks ahead of it, one for music and one for effects, each into its own bounded
    // queue; a block that is not ready in time plays as silence and the other keeps playing.
    // Four fixed slots, so neither side allocates or frees memory.
    struct BlockQueue {
        struct Block {
            uint64_t timestamp = 0;
            AudioBlock samples{};
            uint64_t generation = 0;
        };
        std::mutex mutex;
        std::condition_variable ready;
        std::array<Block, 4> slots;
        size_t first = 0, count = 0;

        void clear() {
            std::lock_guard lock(mutex);
            count = 0;
        }
        // Queues a block unless four already wait or the output has already passed it (`wanted` is
        // the next block it mixes). False means the output will never play this block.
        bool push(uint64_t timestamp, const AudioBlock &samples, uint64_t generation, uint64_t wanted) {
            std::lock_guard lock(mutex);
            dropBefore(wanted);
            if (timestamp < wanted || count == slots.size())
                return false;
            slots[(first + count++) % slots.size()] = {timestamp, samples, generation};
            return true;
        }
        // Takes the block for `timestamp` rendered in `generation`, dropping any older one. Only
        // fixed-size copies happen under the lock, and no worker holds it while rendering. A
        // try-lock would turn harmless microsecond contention into silence.
        bool take(uint64_t timestamp, uint64_t generation, AudioBlock &out) {
            std::lock_guard lock(mutex);
            dropBefore(timestamp);
            while (count && slots[first].generation != generation)
                drop();
            if (!count || slots[first].timestamp != timestamp)
                return false;
            out = slots[first].samples;
            drop();
            return true;
        }
        void dropBefore(uint64_t timestamp) {
            while (count && slots[first].timestamp < timestamp)
                drop();
        }
        void drop() {
            first = (first + 1) % slots.size();
            --count;
        }
        // Taking the lock between storing the wanted timestamp and notifying keeps a worker that is
        // checking its wait condition from missing the change.
        void wake() {
            { std::lock_guard lock(mutex); }
            ready.notify_all();
        }
    };
    BlockQueue musicBlocks, effectsBlocks;
    // Bumped when the source starts again or the output resynchronizes, so a block rendered before
    // that can never reach the speakers after it. The output thread bumps it; workers stamp blocks.
    std::atomic<uint64_t> outputGeneration{0};
    std::atomic<uint64_t> wantedTimestamp{0};
#ifdef OBS_JUKEBOX_QA
    std::atomic<int> testOutputDelayMs{0}, testEffectsDelayMs{0};
    std::atomic<unsigned> testStatCount{0};
    std::atomic<unsigned> testVoiceCount{0};
    std::atomic<unsigned> testMusicResets{0};
    std::atomic<unsigned> testEffectsResets{0};
    std::atomic<uint64_t> testMaxMixNs{0}, testWakeMaxNs{0}, testLateFrames{0};
    // The effects the worker mixed for each recent staged block, for countLateEffects.
    struct DryBlock {
        uint64_t timestamp = 0;
        AudioBlock samples{};
    };
    std::mutex testDryMutex;
    std::array<DryBlock, 8> testDry{};
    std::atomic<unsigned> testMusicMisses{0}, testMusicMissesRaw{0}, testEffectsMisses{0}, testEffectsMissesRaw{0};
    std::atomic<bool> testMeasure{false};
#endif
    explicit SongSource(obs_source_t *s)
        : source(s), linkReceiver(receiver) {}
    struct Voice {
        Decoder decoder;
        std::string loadedPath, resolvedPath;
        unsigned lastEpoch = ~0u;
        bool wasPlaying = false, fileAvailable = false;
        uintmax_t loadedSize = 0;
        std::filesystem::file_time_type loadedWriteTime{};
        Clock::time_point retryAt{};
        uint64_t lastUsed = 0;
        Clock::time_point reopenAt{};
        uint64_t starvedSince = 0;
        Clock::time_point recoverAt{};
        double lastRate = 1;
        float lastGain = 0;              // gain applied to the most recent playing frame
        size_t fadeIn = declickFrames;   // frames of the current fade-in already played
        std::array<float, declickFrames * 2> tail{};
        size_t tailRead = declickFrames; // next tail frame to mix; declickFrames when empty
        PitchChain chain;                // this channel's stages, decoder thread only
        uint64_t chainSession = 0;       // 0 is never a wire session, so the first packet resets

        // Continues the audio that was playing for declickFrames while fading it to silence,
        // so the next state crossfades in instead of cutting. Call before the decoder seeks or
        // reopens. Ignores loop points, which only matters if a loop ends inside those 5 ms.
        void captureTail() {
            if (!wasPlaying)
                return;
            std::array<float, declickFrames * 2> next{};
            decoder.render(next.data(), declickFrames, lastRate);
            for (size_t j = 0; j < declickFrames; ++j) {
                float fade = lastGain * float(declickFrames - 1 - j) / declickFrames;
                for (size_t ch = 0; ch < 2; ++ch) {
                    next[j * 2 + ch] *= fade;
                    if (tailRead + j < declickFrames)
                        next[j * 2 + ch] += tail[(tailRead + j) * 2 + ch];
                }
            }
            tail = next;
            tailRead = 0;
            wasPlaying = false;
        }
        float nextFadeIn() {
            if (fadeIn >= declickFrames)
                return 1;
            return float(++fadeIn) / declickFrames;
        }
        void mixTail(float *output, size_t frames) {
            for (size_t j = 0; j < frames && tailRead < declickFrames; ++j, ++tailRead)
                for (size_t ch = 0; ch < 2; ++ch)
                    output[j * 2 + ch] += tail[tailRead * 2 + ch];
        }
    };
    // Only the decoder thread touches these (the audio tests call renderMusic from their own thread).
    std::array<std::unique_ptr<Voice>, SONG_LINK_MAX_CHANNELS> voices;
    AudioBlock voiceAudio{};
    PitchChain groupChain;         // the music group's stages; decoder thread only
    uint64_t groupSession = 0;
    PitchChain effectsChain{true}; // the SFX stages after the tap; effects worker only, preallocated
    uint64_t effectsSession = 0;
    bool outputActive = false;  // output thread only
    bool outputStarted = false; // output thread only; the first block is not a reactivation
#if defined(__linux__)
    LinuxPaths paths;
#endif

    // Opens the packet's song in the voice when its path changes. At most once a second it also
    // reopens the file if it changed on disk, and retries a failed open every 30 s.
    void followFile(Voice &v, const SongLinkPacket &p) {
        std::string newPath = p.path;
        bool pathChanged = newPath != v.loadedPath;
        if (!pathChanged && (newPath.empty() || Clock::now() < v.retryAt))
            return;
        auto &decoder = v.decoder;
        std::string resolved = newPath;
#if defined(__linux__)
        resolved = paths.resolve(newPath);
#endif
        std::error_code error;
        // Never stat a URL or network share: Decoder::open refuses them, and a stat would reach the network.
        bool local = !resolved.empty() && localSongPath(resolved);
        auto file = std::filesystem::path(std::u8string(resolved.begin(), resolved.end()));
#ifdef OBS_JUKEBOX_QA
        if (local)
            ++testStatCount;
#endif
        auto size = local ? std::filesystem::file_size(file, error) : 0;
        auto modified = !local || error ? std::filesystem::file_time_type{}
                                        : std::filesystem::last_write_time(file, error);
        bool available = local && !error;
        bool fileChanged = resolved != v.resolvedPath || available != v.fileAvailable ||
                           (available && (size != v.loadedSize || modified != v.loadedWriteTime));
        if (pathChanged || fileChanged || (!decoder.ready() && Clock::now() >= v.reopenAt)) {
            v.captureTail();
            decoder.open(resolved);
            v.reopenAt = Clock::now() + std::chrono::seconds(30);
            v.loadedPath = newPath;
            v.lastEpoch = ~0u;
            if (!newPath.empty() && (pathChanged || fileChanged || decoder.ready())) {
                if (decoder.ready())
                    blog(LOG_INFO, "[OBS Jukebox] Decoder ready: %s", p.song);
                else
                    blog(LOG_WARNING, "[OBS Jukebox] Decoder unavailable: %s (%s)", p.song, decoder.error.c_str());
            }
        }
        v.resolvedPath = resolved;
        v.fileAvailable = available;
        v.loadedSize = size;
        v.loadedWriteTime = modified;
        v.retryAt = Clock::now() + std::chrono::seconds(1);
    }
    // Renders `frames` frames of one voice, as packet p describes them from `time`, into
    // `voiceAudio`. A packet that is no longer live only fades out what was playing.
    void renderVoice(Voice &v, const SongLinkPacket &p, size_t frames, uint64_t time, bool live,
                     std::string &decoderError) {
        v.lastUsed = p.timestamp;
        if (!live && !v.wasPlaying && v.tailRead >= declickFrames)
            return;
        followFile(v, p);
        auto &decoder = v.decoder;
        double age = double(int64_t(time) - int64_t(p.timestamp)) / 1e9;
        double sourcePosition = p.position + ((p.flags & 2) ? age * p.rate : 0);
        bool looping = (p.flags & 4) != 0;
        if (looping && sourcePosition >= p.loopEnd)
            sourcePosition = p.loopStart + std::fmod(sourcePosition - p.loopStart, p.loopEnd - p.loopStart);
        double target = sourcePosition + p.offset;
        bool playing = live && (p.flags & 3) == 3 && active && std::isfinite(target) && target >= 0 &&
                       target <= 31536000.0 && decoder.ready();
        const bool expectedAudio = playing && decoder.hasAudioAt(target);
        bool starved = false;
        bool seek = playing && (v.lastEpoch != p.epoch || !v.wasPlaying || std::abs(decoder.position - target) > .04);
        if (!playing || seek)
            v.captureTail();
        voiceAudio.fill(0);
        if (seek) {
            playing = decoder.seek(target);
            v.fadeIn = 0;
        }
        // Errors under the 40 ms seek threshold (FMOD's output clock drifting against
        // OBS's) are steered out by playing up to 0.5% fast or slow, under 9 cents.
        double rate = p.rate * (1 + std::clamp((target - decoder.position) * .5, -.005, .005));
        bool audible = playing;
        if (playing) {
            size_t rendered = 0;
            while (rendered < frames) {
                size_t count = frames - rendered;
                if (looping) {
                    double untilEnd = (p.loopEnd - sourcePosition) * blockRate / rate;
                    count = std::min(count, size_t(std::max(1.0, std::ceil(untilEnd))));
                }
                decoder.render(voiceAudio.data() + rendered * 2, count, rate);
                starved |= decoder.starvedBeforeEnd();
                rendered += count;
                sourcePosition += double(count) * rate / blockRate;
                if (looping && sourcePosition >= p.loopEnd) {
                    sourcePosition =
                        p.loopStart + std::fmod(sourcePosition - p.loopStart, p.loopEnd - p.loopStart);
                    if (!decoder.seek(sourcePosition + p.offset)) {
                        playing = false;
                        break;
                    }
                }
            }
        }
        if (expectedAudio && (!playing || starved)) {
            if (!v.starvedSince)
                v.starvedSince = time;
            if (time >= v.starvedSince + 100000000 && Clock::now() >= v.recoverAt) {
                // Recreate a backend that accepted a seek but stopped
                // delivering PCM. The next block seeks from its fresh
                // timestamp, including rate and user offset; it never
                // resumes from the position before the stall.
                v.recoverAt = Clock::now() + std::chrono::seconds(1);
                decoder.open(v.resolvedPath);
                v.lastEpoch = ~0u;
                playing = false;
                v.starvedSince = 0;
                blog(LOG_WARNING, "[OBS Jukebox] Recovering stalled music decoder on channel %d", p.channelID);
            }
        } else {
            v.starvedSince = 0;
        }
        if (audible)
            for (size_t j = 0; j < frames; ++j) {
                float gain = p.musicVolume * songGainAt(p, frameTime(time, j)) * v.nextFadeIn();
                for (size_t ch = 0; ch < 2; ++ch)
                    voiceAudio[j * 2 + ch] *= gain;
                v.lastGain = gain;
            }
        v.mixTail(voiceAudio.data(), frames);
        if (p.path[0] && !decoder.error.empty())
            decoderError = decoder.error;
        v.wasPlaying = playing;
        v.lastEpoch = p.epoch;
        v.lastRate = rate;
    }
    // Drops what the decoder thread's stages hold for blocks that will never reach the speakers.
    void resetMusicChains() {
#ifdef OBS_JUKEBOX_QA
        ++testMusicResets;
#endif
        for (auto &voice : voices)
            if (voice) {
                voice->chain.reset();
                voice->chainSession = 0;
            }
        groupChain.reset();
        groupSession = 0;
    }
    // Drops what the effects stages hold across a gap in the effects worker's blocks.
    void resetEffectsChain() {
#ifdef OBS_JUKEBOX_QA
        ++testEffectsResets;
#endif
        effectsChain.reset();
        effectsSession = 0;
    }
    // Mixes every voice's music for the block starting at `timestamp` and updates the status
    // OBS shows in the source's properties. `now` is os_gettime_ns().
    void renderMusic(uint64_t timestamp, AudioBlock &audio, uint64_t now) {
        std::string decoderError;
        auto link = linkReceiver->get();
        auto latest = link.packet;
        bool connected = link.connected && Clock::now() - link.received < std::chrono::milliseconds(400);
        bool stalled = linkReceiver->mainThreadStalled(now);
        AudioBlock eligible{}, excluded{};
        for (int channel : linkReceiver->getChannels()) {
            if (channel < 0)
                continue;
            // A packet goes stale 400 ms after its timestamp; that stretch is still rendered, with
            // live=false, so a playing voice fades out instead of cutting off. Nothing goes stale
            // while GD's main thread is stalled but its mixer still plays.
            auto window = linkReceiver->playbackWindow(timestamp, timestamp + blockNs, channel);
            // A channel with no voice and only stale packets has nothing to play or fade out.
            if (!voices[channel] && !stalled &&
                std::none_of(window.begin(), window.end(),
                             [&](const SongLinkPacket &p) { return p.timestamp + staleNs > timestamp; }))
                continue;
            if (!voices[channel]) {
                voices[channel] = std::make_unique<Voice>();
#ifdef OBS_JUKEBOX_QA
                ++testVoiceCount;
#endif
            }
            auto &v = *voices[channel];
            forEachRun(window, timestamp, [&](const SongLinkPacket &p, size_t first, size_t end) {
                size_t stale = stalled ? end : std::clamp(frameAt(timestamp, p.timestamp + staleNs), first, end);
                auto renderPart = [&](size_t from, size_t to, bool live) {
                    const size_t frames = to - from;
                    voiceAudio.fill(0);
                    renderVoice(v, p, frames, frameTime(timestamp, from), live, decoderError);
                    processStages(v.chain, v.chainSession, p.sessionID, voiceAudio.data(), frames, p.stages,
                                  p.stageCount);
                    float *bus = (p.flags & 8) ? excluded.data() : eligible.data();
                    for (size_t j = 0; j < frames * 2; ++j)
                        bus[from * 2 + j] += voiceAudio[j];
                };
                if (stale > first)
                    renderPart(first, stale, true);
                if (end > stale)
                    renderPart(stale, end, false);
            });
        }
        for (auto &voice : voices)
            if (voice && !stalled && timestamp > voice->lastUsed + staleNs)
                voice.reset();
        // The music group's stages, once on the sum of the eligible voices. A replacement voice
        // that the setting excludes never reaches this bus.
        auto global = linkReceiver->playbackWindow(timestamp, timestamp + blockNs, -1);
        forEachRun(global, timestamp, [&](const SongLinkPacket &p, size_t first, size_t end) {
            processStages(groupChain, groupSession, p.sessionID, eligible.data() + first * 2, end - first,
                          p.stages, p.stageCount);
        });
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] = eligible[i] + excluded[i];
        bool oldMod = !connected && linkReceiver->oldModSending(now);
        bool newMod = !connected && linkReceiver->newModSending(now);
        std::lock_guard lock(mutex);
        detail = oldMod ? "Update the OBS Jukebox mod in Geometry Dash to match this plugin."
                 : newMod ? "Update the OBS Jukebox plugin to match the mod in Geometry Dash."
                 : !linkReceiver->available() ? "Game link unavailable: local UDP port is already in use."
                 : !decoderError.empty() ? decoderError
                 : !connected            ? "Waiting for Geometry Dash"
                 : !latest.song[0]       ? noSongDetail
                                         : std::string(latest.song) + " | " + latest.status +
                                               (latest.attempt > 0 ? " | Attempt " + std::to_string(latest.attempt)
                                                                   : "");
    }
    // A worker's loop: renders consecutive blocks into `queue`, at most `lead` blocks ahead of the
    // block the output thread mixes next. Whenever the blocks it renders stop following each other on
    // the output (the source stops or starts, the output resynchronizes, a block is never played),
    // what the stages hold belongs to audio OBS will not play, so `reset` starts them over.
    template <class Render, class Reset>
    void produceBlocks(BlockQueue &queue, uint64_t lead, Render render, Reset reset) {
        uint64_t timestamp = wantedTimestamp.load(), generation = outputGeneration.load();
        bool wasActive = false;
        while (!stop) {
            const bool nowActive = active.load();
            const auto current = outputGeneration.load();
            if (nowActive != wasActive || current != generation) {
                reset();
                if (nowActive)
                    queue.clear(); // a block rendered before the source was deactivated must not play
                wasActive = nowActive;
                generation = current;
            }
            const auto wanted = wantedTimestamp.load();
            if (timestamp < wanted || (timestamp - wanted) % blockNs != 0) {
                timestamp = wanted;
                reset();
                queue.clear();
            }
            if (timestamp > wanted + lead * blockNs) {
                std::unique_lock lock(queue.mutex);
                queue.ready.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return stop || timestamp <= wantedTimestamp.load() + lead * blockNs;
                });
                continue;
            }
            AudioBlock audio{};
            render(timestamp, audio);
            if (!queue.push(timestamp, audio, generation, wantedTimestamp.load()))
                reset();
            timestamp += blockNs;
        }
    }
    // Music decodes up to two blocks ahead, since opening, seeking and decoding take variable time.
    void decodeMusic() {
        configureAudioThread();
        produceBlocks(musicBlocks, 2, [&](uint64_t t, AudioBlock &audio) { renderMusic(t, audio, os_gettime_ns()); },
                      [&] { resetMusicChains(); });
        // On Windows each decoder started COM on this thread when it opened, so it has to close here too.
        for (auto &voice : voices)
            voice.reset();
    }
    // Effects with stages render one block ahead of the output: enough for an output thread that
    // wakes on 15.6 ms timer ticks and mixes two blocks at once. Effects arrive over the network just
    // in time, so that lead costs margin: the last frame of a staged block has to arrive within 10 ms
    // of its timestamp instead of 30. A block without stages never waits for this worker; the output
    // thread mixes it itself (mixBlock), with the full margin.
    void renderEffectsBlocks() {
        configureAudioThread();
        produceBlocks(effectsBlocks, 1, [&](uint64_t t, AudioBlock &audio) {
            if (active)
                renderEffects(t, audio);
        }, [&] { resetEffectsChain(); });
    }
    // Adds the game's effects for each run of the global window. Only the global packet carries the
    // game's effects state; a voice packet never does, so with no global history nothing is mixed.
    // SFX never waits for music decoding, opening, seeking or path resolution. Effects ignore
    // staleness: they arrive from FMOD's mixer thread even while GD's main thread (which sends
    // status packets) stalls, and stop on their own when GD does.
    void mixGameEffects(const std::vector<SongLinkPacket> &window, uint64_t timestamp, AudioBlock &effects) {
        forEachRun(window, timestamp, [&](const SongLinkPacket &p, size_t first, size_t end) {
            if (p.flags & 1)
                linkReceiver->mixEffects(effects.data() + first * 2, end - first, frameTime(timestamp, first),
                                         p.effectsVolume);
        });
    }
    static bool hasEffectsStages(const std::vector<SongLinkPacket> &window, uint64_t timestamp) {
        bool staged = false;
        forEachRun(window, timestamp,
                   [&](const SongLinkPacket &p, size_t, size_t) { staged |= p.effectsStageCount != 0; });
        return staged;
    }
    // The game's effects for a block with SFX stages, through those stages. A block without stages
    // stays silent here, since mixBlock mixes it itself; its runs still pass through the chain, so
    // a stage that comes back starts fresh.
    void renderEffects(uint64_t timestamp, AudioBlock &effects) {
#ifdef OBS_JUKEBOX_QA
        std::this_thread::sleep_for(std::chrono::milliseconds(testEffectsDelayMs.exchange(0)));
#endif
        const auto window = linkReceiver->playbackWindow(timestamp, timestamp + blockNs, -1);
        if (hasEffectsStages(window, timestamp)) {
            mixGameEffects(window, timestamp, effects);
#ifdef OBS_JUKEBOX_QA
            std::lock_guard lock(testDryMutex);
            testDry[timestamp / blockNs % testDry.size()] = {timestamp, effects};
#endif
        }
        forEachRun(window, timestamp, [&](const SongLinkPacket &p, size_t first, size_t end) {
            processStages(effectsChain, effectsSession, p.sessionID, effects.data() + first * 2, end - first,
                          p.effectsStages, p.effectsStageCount);
        });
    }
#ifdef OBS_JUKEBOX_QA
    // Counts the frames of a staged block whose effects packets arrived after the worker rendered
    // it: mixed again now, they differ from what the worker mixed.
    void countLateEffects(const std::vector<SongLinkPacket> &window, uint64_t timestamp) {
        AudioBlock now{};
        mixGameEffects(window, timestamp, now);
        std::lock_guard lock(testDryMutex);
        const auto &rendered = testDry[timestamp / blockNs % testDry.size()];
        if (rendered.timestamp != timestamp)
            return;
        for (size_t f = 0; f < blockFrames; ++f)
            testLateFrames += std::abs(now[f * 2] - rendered.samples[f * 2]) > 1e-6f ||
                              std::abs(now[f * 2 + 1] - rendered.samples[f * 2 + 1]) > 1e-6f;
    }
#endif
    // The output block starting at `timestamp`: the music worker's block and the effects, each
    // silent if it is not ready in time.
    AudioBlock mixBlock(uint64_t timestamp) {
        const bool nowActive = active.load();
        if (!outputStarted) {
            outputActive = nowActive;
            outputStarted = true;
        } else if (nowActive != outputActive) {
            // A source that was deactivated must not replay audio it held before the gap. Bumping
            // the generation also rejects a block a worker rendered before it saw the change.
            if (nowActive)
                outputGeneration.fetch_add(1);
            musicBlocks.clear();
            effectsBlocks.clear();
            outputActive = nowActive;
        }
        AudioBlock audio{}, effects{};
        const auto generation = outputGeneration.load();
        // Only the QA miss counters read whether each block arrived; a missing one is silence.
        [[maybe_unused]] const bool hasMusic = musicBlocks.take(timestamp, generation, audio);
        [[maybe_unused]] const bool hasEffects = effectsBlocks.take(timestamp, generation, effects);
        if (!nowActive)
            return {};
        // Effects without SFX stages are only a mix, cheap enough for this thread, and mixing them
        // here keeps the full margin for late packets. Staged blocks come from the worker. The newest
        // packets decide: a stage that appears after the worker rendered its block leaves that block
        // silent, as a fresh FMOD shifter is, and one removed since then is gone at once.
        const auto window = linkReceiver->playbackWindow(timestamp, timestamp + blockNs, -1);
        const bool staged = hasEffectsStages(window, timestamp);
        if (!staged) {
            effects = {};
            mixGameEffects(window, timestamp, effects);
        }
#ifdef OBS_JUKEBOX_QA
        const bool measured = testMeasure.load(std::memory_order_relaxed);
        if (!hasMusic) {
            ++testMusicMissesRaw;
            testMusicMisses += measured;
        }
        if (staged && !hasEffects) {
            ++testEffectsMissesRaw;
            testEffectsMisses += measured;
        }
        if (staged && hasEffects)
            countLateEffects(window, timestamp);
#endif
        for (size_t i = 0; i < audio.size(); ++i)
            audio[i] += effects[i];
        // A corrupt float WAV or a hostile packet must not put NaN or Inf into OBS's mix.
        for (auto &sample : audio)
            if (!std::isfinite(sample))
                sample = 0;
        return audio;
    }
    void run() {
        configureAudioThread();
        // Prefill one block while retaining the existing 40 ms output timeline delay.
        uint64_t timestamp = os_gettime_ns() - bufferingNs + blockNs;
        wantedTimestamp = timestamp;
        decoderWorker = std::thread([this] { decodeMusic(); });
        effectsWorker = std::thread([this] { renderEffectsBlocks(); });
        auto next = Clock::now() + std::chrono::nanoseconds(blockNs);
        while (!stop) {
            std::this_thread::sleep_until(next);
#ifdef OBS_JUKEBOX_QA
            qaMax(testWakeMaxNs, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - next)
                                             .count()));
            std::this_thread::sleep_for(std::chrono::milliseconds(testOutputDelayMs.exchange(0)));
            const auto mixBegin = std::chrono::steady_clock::now();
#endif
            auto audio = mixBlock(timestamp);
#ifdef OBS_JUKEBOX_QA
            const auto mixNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - mixBegin)
                                           .count());
            qaMax(testMaxMixNs, mixNs);
#endif
            obs_source_audio block{};
            block.data[0] = reinterpret_cast<const uint8_t *>(audio.data());
            block.frames = blockFrames;
            block.speakers = SPEAKERS_STEREO;
            block.format = AUDIO_FORMAT_FLOAT;
            block.samples_per_sec = blockRate;
            block.timestamp = timestamp;
            obs_source_output_audio(source, &block);
            timestamp += blockNs;
            next += std::chrono::nanoseconds(blockNs);
            auto now = Clock::now();
            if (next < now - std::chrono::milliseconds(100)) {
                next = now;
                timestamp = os_gettime_ns() - bufferingNs;
                // Output blocks were skipped; nothing staged for them may play. The workers reset
                // their chains when they see the new generation.
                outputGeneration.fetch_add(1);
                musicBlocks.clear();
                effectsBlocks.clear();
            }
            wantedTimestamp = timestamp;
            musicBlocks.wake();
            effectsBlocks.wake();
        }
        for (auto *thread : {&decoderWorker, &effectsWorker})
            if (thread->joinable())
                thread->join();
    }
    ~SongSource() {
        stop = true;
        musicBlocks.wake();
        effectsBlocks.wake();
        if (worker.joinable())
            worker.join();
    }
};

static const char *sourceName(void *) { return "GD Sounds"; }
static void *create(obs_data_t *, obs_source_t *source) {
    auto s = new SongSource(source);
    s->worker = std::thread([s] { s->run(); });
    return s;
}
static obs_properties_t *properties(void *data) {
    auto props = obs_properties_create();
    obs_properties_add_text(props, "version", "OBSJukebox " OBS_JUKEBOX_VERSION, OBS_TEXT_INFO);
    std::string detail = "Playback follows GD automatically.";
    if (data) {
        auto s = static_cast<SongSource *>(data);
        std::lock_guard lock(s->mutex);
        detail = s->detail;
    }
    obs_properties_add_text(props, "link", detail.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "help",
                            "Includes all sounds through GD, including selected custom song. Mute audio on "
                            "other GD/desktop captures to prevent duplicated audio.",
                            OBS_TEXT_INFO);
    return props;
}
bool obs_module_load(void) {
    // Build the shifter tables before any source thread can process audio.
    PitchShifter::prepareTables();
    receiver = std::make_shared<Receiver>();
    obs_source_info info{};
    info.id = "gd_alternate_song";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = sourceName;
    info.create = create;
    info.destroy = [](void *d) { delete static_cast<SongSource *>(d); };
    info.get_properties = properties;
    info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
    info.activate = [](void *d) { static_cast<SongSource *>(d)->active = true; };
    info.deactivate = [](void *d) { static_cast<SongSource *>(d)->active = false; };
    obs_register_source(&info);
    blog(LOG_INFO, "[OBS Jukebox] Native GD Sounds source registered");
    return true;
}
void obs_module_unload(void) { receiver.reset(); }
