#include "../src/LinkPacket.hpp"
#include "../src/Socket.hpp"
#include "Decoder.hpp"
#include "LinkClock.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <obs-module.h>
#include <string>
#include <thread>
#include <util/platform.h>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#if defined(__linux__)
#include "LinuxAudioClock.hpp"
#include "LinuxPaths.hpp"
#endif

OBS_DECLARE_MODULE()
MODULE_EXPORT const char *obs_module_description(void) {
    return "OBS Jukebox: Geometry Dash custom song and sound effects";
}
MODULE_EXPORT const char *obs_module_name(void) { return "OBS Jukebox"; }
MODULE_EXPORT const char *obs_module_author(void) { return "babbur"; }

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
    uint64_t session = 0, lastProbe = 0, lastSongReceived = 0;
    std::array<uint64_t, 2> lastEffectsReceived{};
    std::array<uint16_t, 2> effectsPort{};
    static constexpr uint64_t sessionTimeoutNs = 2000000000ULL;
    std::deque<uint64_t> probes;
    sockaddr_in peer{};
    std::deque<SongLinkPacket> pending;
#if defined(__linux__)
    LinuxAudioClock effectsClock;
#endif
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
        else if (p.version >= 5 && !clock.translate(p.timestamp))
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

  public:
    Receiver() {
        if (!socketsReady())
            return;
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(receiverPort());
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
        worker = std::thread([this] {
            FILE *reference = nullptr;
#ifdef OBS_JUKEBOX_QA
            if (auto path = std::getenv("SEPARATE_SONG_REFERENCE_FILE"))
                reference = std::fopen(path, "wb");
#endif
            while (!stop) {
                std::array<unsigned char, sizeof(EffectsPacketV2) + 1> buffer{};
                sockaddr_in from{};
#ifdef _WIN32
                int length = sizeof(from);
#else
                socklen_t length = sizeof(from);
#endif
                auto size = recvfrom(sock, reinterpret_cast<char *>(buffer.data()), buffer.size(), 0,
                                     reinterpret_cast<sockaddr *>(&from), &length);
                auto now = os_gettime_ns();
                probe(now);
                if (size <= 0) {
                    waitSocketReadable(sock, 50);
                    continue;
                }
                if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
                    continue;
                if (size == sizeof(ClockSyncPacket) && !memcmp(buffer.data(), "GDCLK1\0", 8)) {
                    ClockSyncPacket p;
                    memcpy(&p, buffer.data(), sizeof(p));
                    auto found = std::find(probes.begin(), probes.end(), p.t1);
                    if (p.version != 1 || p.kind != 2 || p.sessionID != session || found == probes.end() ||
                        from.sin_port != peer.sin_port)
                        continue;
                    probes.erase(found);
                    bool changed = false;
                    std::lock_guard lock(mutex);
                    if (clock.observe(p, now, changed)) {
                        if (changed)
                            clearTimeline();
                        for (auto &waiting : pending)
                            accept(waiting, now);
                        pending.clear();
                        for (auto waiting : pendingEffects)
                            if (clock.translate(waiting.timestamp) && waiting.timestamp <= now + 1000000000 &&
                                waiting.timestamp + 1000000000 >= now)
                                queueEffects(waiting);
                        pendingEffects.clear();
                        while (effects.size() > 128)
                            effects.pop_front();
                    }
                    continue;
                }
                bool modernEffects = size >= 44 && !memcmp(buffer.data(), "GDSFX2\0", 8);
                if (modernEffects || (size >= 36 && !memcmp(buffer.data(), "GDSFX1\0", 8))) {
                    EffectsPacket effect;
                    uint64_t effectSession = 0;
                    if (modernEffects) {
                        if (size > sizeof(EffectsPacketV2))
                            continue;
                        EffectsPacketV2 p;
                        memcpy(&p, buffer.data(), size);
                        if (p.version != 2 || p.frames > 512 || size != 44 + p.frames * 8)
                            continue;
                        effect.sequence = p.sequence;
                        effect.sampleRate = p.sampleRate;
                        effect.frames = p.frames;
                        effect.timestamp = p.timestamp;
                        effect.stream = p.stream;
                        effectSession = p.sessionID;
                        std::copy_n(p.samples, p.frames * 2, effect.samples);
                    } else {
                        if (size > sizeof(effect))
                            continue;
                        memcpy(&effect, buffer.data(), size);
                        if (effect.version != 1 || effect.frames > 512 || size != 36 + effect.frames * 8)
                            continue;
                    }
                    if (effect.frames < 1 || effect.sampleRate < 8000 || effect.sampleRate > 192000 ||
                        effect.stream > 1)
                        continue;
#ifndef OBS_JUKEBOX_QA
                    if (effect.stream != 0)
                        continue;
#endif
                    bool finite = true;
                    for (unsigned i = 0; i < effect.frames * 2; ++i)
                        finite &= std::isfinite(effect.samples[i]);
                    if (!finite)
                        continue;
                    std::lock_guard lock(mutex);
                    if (modernEffects) {
                        if (!session || effectSession != session)
                            continue;
                        // The effects tap uses its own socket. Pin its port independently
                        // and permit a replacement tap only after the previous one is stale.
                        if (effectsPort[effect.stream] && effectsPort[effect.stream] != from.sin_port &&
                            now - lastEffectsReceived[effect.stream] <= sessionTimeoutNs)
                            continue;
                        effectsPort[effect.stream] = from.sin_port;
                        lastEffectsReceived[effect.stream] = now;
                        if (!clock.valid()) {
                            if (effect.stream == 0) {
                                pendingEffects.push_back(effect);
                                while (pendingEffects.size() > 128)
                                    pendingEffects.pop_front();
                            }
                            continue;
                        }
                        if (!clock.translate(effect.timestamp))
                            continue;
                    } else {
                        // v2-v4 song packets and GDSFX1 remain compatible with pre-clock-sync
                        // releases, but may never inject audio into an active v5 session.
                        if (session || !lastSongReceived || from.sin_addr.s_addr != peer.sin_addr.s_addr)
                            continue;
                        if (effectsPort[effect.stream] && effectsPort[effect.stream] != from.sin_port &&
                            now - lastEffectsReceived[effect.stream] <= sessionTimeoutNs)
                            continue;
                        effectsPort[effect.stream] = from.sin_port;
                        lastEffectsReceived[effect.stream] = now;
#if defined(__linux__)
                        if (effectsClock.translate(effect.timestamp, now))
                            effects.clear();
#endif
                    }
                    if (effect.timestamp > now + 1000000000 || effect.timestamp + 1000000000 < now)
                        continue;
                    if (effect.stream == 1) {
                        if (reference) {
                            std::fwrite(&effect, sizeof(effect), 1, reference);
                            std::fflush(reference);
                        }
                        continue;
                    }
                    queueEffects(effect);
                    continue;
                }
                SongLinkPacket p{};
                if (size != sizeof(p) && size != SONG_LINK_V4_SIZE && size != SONG_LINK_V3_SIZE &&
                    size != SONG_LINK_V2_SIZE)
                    continue;
                memcpy(&p, buffer.data(), size);
                if (!((size == sizeof(p) && p.version == 5) ||
                      (size == SONG_LINK_V4_SIZE && p.version == 4) ||
                      (size == SONG_LINK_V3_SIZE && p.version == 3) ||
                      (size == SONG_LINK_V2_SIZE && p.version == 2)) ||
                    memcmp(p.magic, "GDSONG1", 8))
                    continue;
                if (!std::isfinite(p.musicVolume) || p.musicVolume < 0 || p.musicVolume > 1 ||
                    !std::isfinite(p.effectsVolume) || p.effectsVolume < 0 || p.effectsVolume > 1 ||
                    !std::isfinite(p.position) || p.position < -600 || p.position >= 86400 ||
                    !std::isfinite(p.offset) || !std::isfinite(p.rate) || p.rate < .25 || p.rate > 4 ||
                    p.channelID < -1 || p.channelID >= SONG_LINK_MAX_CHANNELS ||
                    !std::isfinite(p.triggerGain) || p.triggerGain < 0 || p.triggerGain > 16 ||
                    p.fadeCount > 8)
                    continue;
                bool valid = true;
                uint64_t previous = 0;
                for (unsigned i = 0; i < p.fadeCount; ++i) {
                    const auto &f = p.fades[i];
                    if (f.offsetNs < previous || f.offsetNs > 86400000000000ULL || !std::isfinite(f.gain) ||
                        f.gain < 0 || f.gain > 16)
                        valid = false;
                    previous = f.offsetNs;
                }
                if ((p.flags & 4) && (!std::isfinite(p.loopStart) || !std::isfinite(p.loopEnd) ||
                                      p.loopStart < 0 || p.loopEnd <= p.loopStart || p.loopEnd > 86400))
                    valid = false;
                if (!valid)
                    continue;
                p.status[23] = p.level[95] = p.song[95] = p.path[1023] = 0;
                if (p.version < 4)
                    p.timestamp = now;
                std::lock_guard lock(mutex);
                if (p.version >= 5) {
                    if (!p.sessionID)
                        continue;
                    if (session != p.sessionID || from.sin_port != peer.sin_port) {
                        if (lastSongReceived && now - lastSongReceived <= sessionTimeoutNs)
                            continue;
                        blog(LOG_INFO, "[OBS Jukebox] Adopting game session %llu",
                             static_cast<unsigned long long>(p.sessionID));
                        session = p.sessionID;
                        peer = from;
                        clock.reset();
                        pending.clear();
                        pendingEffects.clear();
                        probes.clear();
                        lastProbe = 0;
                        effectsPort.fill(0);
                        lastEffectsReceived.fill(0);
                        clearTimeline();
                    }
                    lastSongReceived = now;
                    if (!clock.valid()) {
                        pending.push_back(p);
                        while (pending.size() > 256)
                            pending.pop_front();
                        probe(now);
                        continue;
                    }
                } else {
                    if (lastSongReceived && (session || from.sin_port != peer.sin_port) &&
                        now - lastSongReceived <= sessionTimeoutNs)
                        continue;
                    if (session || (lastSongReceived && from.sin_port != peer.sin_port)) {
                        session = 0;
                        clock.reset();
                        pending.clear();
                        pendingEffects.clear();
                        probes.clear();
                        effectsPort.fill(0);
                        lastEffectsReceived.fill(0);
                        clearTimeline();
                    }
                    peer = from;
                    lastSongReceived = now;
                }
                accept(p, now);
            }
            if (reference)
                std::fclose(reference);
        });
    }
    ~Receiver() {
        stop = true;
        if (worker.joinable())
            worker.join();
        if (sock != BAD_SOCKET)
            closeSocket(sock);
    }
    bool available() const { return sock != BAD_SOCKET; }
    // GD's main thread sends song packets and FMOD's mixer thread sends effects. Effects that keep
    // arriving after song packets stop mean the main thread is stalled (as while GD tears down a
    // level), and the game is still playing its music.
    bool mainThreadStalled() {
        std::lock_guard lock(mutex);
        auto now = os_gettime_ns();
        return lastSongReceived && lastEffectsReceived[0] && now - lastEffectsReceived[0] < 100000000 &&
               now - lastSongReceived > 200000000;
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
            if (delta >= length || delta + double(frames) / 48000 < 0)
                continue;
            for (size_t i = 0; i < frames; ++i) {
                double time = delta + double(i) / 48000;
                if (time < 0 || time >= length)
                    continue;
                unsigned a = std::min(unsigned(time * p.sampleRate), p.frames - 1);
                bool seam = a + 1 == p.frames;
                double blend = seam ? (next ? (time - lastTime) / (length - lastTime) : 0)
                                    : time * p.sampleRate - a;
                for (unsigned ch = 0; ch < 2; ++ch) {
                    float from = p.samples[a * 2 + ch],
                          to = seam ? (next ? next->samples[ch] : from) : p.samples[(a + 1) * 2 + ch];
                    output[i * 2 + ch] += gain * float(from + (to - from) * blend);
                }
            }
        }
    }
};
static std::shared_ptr<Receiver> receiver;

// Length of the crossfade at every music discontinuity (pause, resume, restart, seek, song
// change): 5 ms at 48 kHz. FMOD ramps these in game; without it OBS hears a click.
constexpr size_t declickFrames = 240;

struct SongSource {
    obs_source_t *source;
    std::atomic<bool> stop{false}, active{false};
    std::mutex mutex;
    std::string detail;
    std::thread worker, decoderWorker;
    // The output thread owns no decoder or filesystem object. A bounded queue
    // carries timestamped music blocks; missed deadlines produce silence while SFX continues.
    std::shared_ptr<Receiver> linkReceiver;
    static constexpr uint64_t blockNs = 10000000, bufferingNs = 40000000, staleNs = 400000000;
    struct MusicBlock {
        uint64_t timestamp;
        std::array<float, 960> samples{};
    };
    std::mutex musicMutex;
    std::condition_variable musicReady;
    std::deque<MusicBlock> musicQueue;
    std::atomic<uint64_t> wantedTimestamp{0};
#ifdef OBS_JUKEBOX_QA
    std::atomic<int> testOutputDelayMs{0};
#endif
    explicit SongSource(obs_source_t *s)
        : source(s), linkReceiver(receiver), detail("Select the OBS checkbox beside a Jukebox song.") {}
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
    void decodeMusic() {
        configureAudioThread();
        std::array<std::unique_ptr<Voice>, SONG_LINK_MAX_CHANNELS> voices;
#if defined(__linux__)
        LinuxPaths paths;
#endif
        std::array<float, 960> audio{}, voiceAudio{};
        uint64_t timestamp = wantedTimestamp.load();
        while (!stop) {
            auto wanted = wantedTimestamp.load();
            if (timestamp < wanted || (timestamp - wanted) % blockNs != 0) {
                timestamp = wanted;
                std::lock_guard lock(musicMutex);
                musicQueue.clear();
            }
            if (timestamp > wanted + 2 * blockNs) {
                std::unique_lock lock(musicMutex);
                musicReady.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return stop || timestamp <= wantedTimestamp.load() + 2 * blockNs;
                });
                continue;
            }
            audio.fill(0);
            std::string decoderError;
            auto link = linkReceiver->get();
            auto latest = link.packet;
            bool connected = link.connected && Clock::now() - link.received < std::chrono::milliseconds(400);
            bool stalled = linkReceiver->mainThreadStalled();
            auto channels = linkReceiver->getChannels();
            auto frameAt = [&](uint64_t at) {
                return at <= timestamp ? size_t(0)
                                       : size_t(std::min<uint64_t>(
                                             480, ((at - timestamp) * 48000 + 999999999) / 1000000000));
            };
            auto frameTime = [&](size_t frame) { return timestamp + frame * 1000000000 / 48000; };
            // Calls render(packet, first, end, time, live) for each run of frames one packet describes.
            // A packet goes stale 400 ms after its timestamp; that stretch is still passed, with
            // live=false, so a playing voice fades out instead of cutting off. Nothing goes stale
            // while GD's main thread is stalled but its mixer still plays.
            auto segments = [&](int channel, auto render) {
                auto window = linkReceiver->playbackWindow(timestamp, timestamp + 10000000, channel);
                for (size_t i = 0; i < window.size(); ++i) {
                    auto &p = window[i];
                    if (!p.timestamp)
                        continue;
                    size_t first = i ? frameAt(p.timestamp) : 0;
                    size_t end = i + 1 < window.size() ? frameAt(window[i + 1].timestamp) : 480;
                    size_t stale = stalled ? end : std::clamp(frameAt(p.timestamp + 400000000), first, end);
                    if (stale > first)
                        render(p, first, stale, frameTime(first), true);
                    if (end > stale)
                        render(p, stale, end, frameTime(stale), false);
                }
            };
            for (int channel : channels) {
                if (channel < 0)
                    continue;
                if (!voices[channel])
                    voices[channel] = std::make_unique<Voice>();
                auto &v = *voices[channel];
                auto &decoder = v.decoder;
                segments(channel, [&](const SongLinkPacket &p, size_t first, size_t end,
                                      uint64_t segmentTime, bool live) {
                    v.lastUsed = p.timestamp;
                    // A stale packet only fades out what was playing.
                    if (!live && !v.wasPlaying && v.tailRead >= declickFrames)
                        return;
                    std::string newPath = p.path;
                    bool pathChanged = newPath != v.loadedPath;
                    if (pathChanged || (!newPath.empty() && Clock::now() >= v.retryAt)) {
                        std::string resolved = newPath;
#if defined(__linux__)
                        resolved = paths.resolve(newPath);
#endif
                        std::error_code error;
                        auto file = std::filesystem::path(std::u8string(resolved.begin(), resolved.end()));
                        auto size = resolved.empty() ? 0 : std::filesystem::file_size(file, error);
                        auto modified = resolved.empty() || error
                                            ? std::filesystem::file_time_type{}
                                            : std::filesystem::last_write_time(file, error);
                        bool available = !resolved.empty() && !error;
                        bool fileChanged =
                            resolved != v.resolvedPath || available != v.fileAvailable ||
                            (available && (size != v.loadedSize || modified != v.loadedWriteTime));
                        if (pathChanged || fileChanged || (!decoder.ready() && Clock::now() >= v.reopenAt)) {
                            v.captureTail();
                            decoder.open(resolved);
                            v.reopenAt = Clock::now() + std::chrono::seconds(30);
                            v.loadedPath = newPath;
                            v.lastEpoch = ~0u;
                            if (!newPath.empty() && (pathChanged || fileChanged || decoder.ready()))
                                blog(LOG_INFO, "[OBS Jukebox] Decoder %s: %s",
                                     decoder.ready() ? "ready" : "unavailable", p.song);
                        }
                        v.resolvedPath = resolved;
                        v.fileAvailable = available;
                        v.loadedSize = size;
                        v.loadedWriteTime = modified;
                        v.retryAt = Clock::now() + std::chrono::seconds(1);
                    }
                    double age = double(int64_t(segmentTime) - int64_t(p.timestamp)) / 1e9;
                    double sourcePosition = p.position + ((p.flags & 2) ? age * p.rate : 0);
                    bool looping = (p.flags & 4) != 0;
                    if (looping && sourcePosition >= p.loopEnd)
                        sourcePosition =
                            p.loopStart + std::fmod(sourcePosition - p.loopStart, p.loopEnd - p.loopStart);
                    double target = sourcePosition + p.offset;
                    bool playing = live && (p.flags & 3) == 3 && active && std::isfinite(target) && target >= 0 &&
                                   target <= 31536000.0 && decoder.ready();
                    const bool expectedAudio = playing && decoder.hasAudioAt(target);
                    bool starved = false;
                    bool seek = playing && (v.lastEpoch != p.epoch || !v.wasPlaying ||
                                            std::abs(decoder.position - target) > .04);
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
                        while (rendered < end - first) {
                            size_t count = end - first - rendered;
                            if (looping) {
                                double untilEnd = (p.loopEnd - sourcePosition) * 48000 / rate;
                                count = std::min(count, size_t(std::max(1.0, std::ceil(untilEnd))));
                            }
                            decoder.render(voiceAudio.data() + rendered * 2, count, rate);
                            starved |= decoder.starvedBeforeEnd();
                            rendered += count;
                            sourcePosition += double(count) * rate / 48000;
                            if (looping && sourcePosition >= p.loopEnd) {
                                sourcePosition = p.loopStart + std::fmod(sourcePosition - p.loopStart,
                                                                         p.loopEnd - p.loopStart);
                                if (!decoder.seek(sourcePosition + p.offset)) {
                                    playing = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (expectedAudio && (!playing || starved)) {
                        if (!v.starvedSince)
                            v.starvedSince = segmentTime;
                        if (segmentTime >= v.starvedSince + 100000000 && Clock::now() >= v.recoverAt) {
                            // Recreate a backend that accepted a seek but stopped
                            // delivering PCM. The next block seeks from its fresh
                            // timestamp, including rate and user offset; it never
                            // resumes from the position before the stall.
                            v.recoverAt = Clock::now() + std::chrono::seconds(1);
                            decoder.open(v.resolvedPath);
                            v.lastEpoch = ~0u;
                            playing = false;
                            v.starvedSince = 0;
                            blog(LOG_WARNING, "[OBS Jukebox] Recovering stalled music decoder on channel %d", channel);
                        }
                    } else {
                        v.starvedSince = 0;
                    }
                    if (audible)
                        for (size_t j = 0; j < end - first; ++j) {
                            float gain = p.musicVolume * songGainAt(p, segmentTime + j * 1000000000 / 48000) *
                                         v.nextFadeIn();
                            for (size_t ch = 0; ch < 2; ++ch)
                                audio[(first + j) * 2 + ch] += voiceAudio[j * 2 + ch] * gain;
                            v.lastGain = gain;
                        }
                    v.mixTail(audio.data() + first * 2, end - first);
                    if (!newPath.empty() && !decoder.error.empty())
                        decoderError = decoder.error;
                    v.wasPlaying = playing;
                    v.lastEpoch = p.epoch;
                    v.lastRate = rate;
                });
            }
            for (auto &voice : voices)
                if (voice && !stalled && timestamp > voice->lastUsed + staleNs)
                    voice.reset();
            {
                std::lock_guard lock(mutex);
                detail = !linkReceiver->available()
                             ? "Game link unavailable: local UDP port is already in use."
                         : !decoderError.empty() ? decoderError
                         : !connected            ? "Waiting for Geometry Dash"
                         : !latest.song[0]       ? "Select the OBS checkbox beside a Jukebox song."
                                                 : std::string(latest.song) + " | " + latest.status +
                                                       (latest.attempt > 0
                                                            ? " | Attempt " + std::to_string(latest.attempt)
                                                            : "");
            }
            {
                std::lock_guard lock(musicMutex);
                while (!musicQueue.empty() && musicQueue.front().timestamp < wantedTimestamp.load())
                    musicQueue.pop_front();
                if (musicQueue.size() < 4)
                    musicQueue.push_back({timestamp, audio});
            }
            timestamp += blockNs;
        }
    }
    void run() {
        configureAudioThread();
        // Prefill one block while retaining the existing 40 ms output timeline delay.
        uint64_t timestamp = os_gettime_ns() - bufferingNs + blockNs;
        wantedTimestamp = timestamp;
        decoderWorker = std::thread([this] { decodeMusic(); });
        auto next = Clock::now() + std::chrono::milliseconds(10);
        while (!stop) {
            std::this_thread::sleep_until(next);
#ifdef OBS_JUKEBOX_QA
            std::this_thread::sleep_for(std::chrono::milliseconds(testOutputDelayMs.exchange(0)));
#endif
            std::array<float, 960> audio{};
            {
                // Only fixed-size PCM copies and at most four queue removals occur
                // under this lock. Decoder/file operations never hold it. A try-lock
                // here would turn harmless microsecond contention into a silent block.
                std::lock_guard lock(musicMutex);
                while (!musicQueue.empty() && musicQueue.front().timestamp < timestamp)
                    musicQueue.pop_front();
                if (!musicQueue.empty() && musicQueue.front().timestamp == timestamp) {
                    if (active)
                        audio = musicQueue.front().samples;
                    musicQueue.pop_front();
                }
            }
            // SFX never waits for music decoding, opening, seeking or path resolution.
            auto channels = linkReceiver->getChannels();
            int global = std::find(channels.begin(), channels.end(), -1) != channels.end() ? -1 : 0;
            auto window = linkReceiver->playbackWindow(timestamp, timestamp + blockNs, global);
            auto frameAt = [&](uint64_t at) {
                return at <= timestamp ? size_t(0)
                                       : size_t(std::min<uint64_t>(
                                             480, ((at - timestamp) * 48000 + 999999999) / 1000000000));
            };
            // Effects ignore staleness: they arrive from FMOD's mixer thread even while GD's main
            // thread (which sends status packets) stalls, and stop on their own when GD does.
            if (active)
                for (size_t i = 0; i < window.size(); ++i) {
                    const auto &packet = window[i];
                    size_t first = i ? frameAt(packet.timestamp) : 0;
                    size_t end = i + 1 < window.size() ? frameAt(window[i + 1].timestamp) : 480;
                    if (packet.timestamp && end > first && (packet.flags & 1))
                        linkReceiver->mixEffects(audio.data() + first * 2, end - first,
                                                 timestamp + first * 1000000000 / 48000,
                                                 packet.effectsVolume);
                }
            obs_source_audio block{};
            block.data[0] = reinterpret_cast<const uint8_t *>(audio.data());
            block.frames = 480;
            block.speakers = SPEAKERS_STEREO;
            block.format = AUDIO_FORMAT_FLOAT;
            block.samples_per_sec = 48000;
            block.timestamp = timestamp;
            obs_source_output_audio(source, &block);
            timestamp += blockNs;
            next += std::chrono::milliseconds(10);
            auto now = Clock::now();
            if (next < now - std::chrono::milliseconds(100)) {
                next = now;
                timestamp = os_gettime_ns() - bufferingNs;
            }
            wantedTimestamp = timestamp;
            musicReady.notify_one();
        }
        if (decoderWorker.joinable())
            decoderWorker.join();
    }
    ~SongSource() {
        stop = true;
        musicReady.notify_all();
        if (worker.joinable())
            worker.join();
    }
};

static const char *sourceName(void *) { return "GD Sounds"; }
static void *create(obs_data_t *settings, obs_source_t *source) {
    auto s = new SongSource(source);
    s->worker = std::thread([s] { s->run(); });
    return s;
}
static obs_properties_t *properties(void *data) {
    auto props = obs_properties_create();
    obs_properties_add_text(props, "selection",
                            "In Jukebox, Game selects the in-game song. OBS selects the OBS song. Click it "
                            "again to clear the OBS song.",
                            OBS_TEXT_INFO);
    std::string detail = "Playback follows GD automatically.";
    if (data) {
        auto s = static_cast<SongSource *>(data);
        std::lock_guard lock(s->mutex);
        detail = s->detail;
    }
    obs_properties_add_text(props, "link", detail.c_str(), OBS_TEXT_INFO);
    obs_properties_add_text(props, "help",
                            "Includes the selected custom song and GD sound effects, including clicks routed "
                            "through GD's effects engine. Keep Audio Monitoring at Monitor Off and disable "
                            "audio on other GD/desktop captures to prevent duplicate music.",
                            OBS_TEXT_INFO);
    return props;
}
bool obs_module_load(void) {
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
