#include "Socket.hpp"
#include "AudioTap.hpp"
#include "AudioDownmix.hpp"
#include "Bridge.hpp"
#include "LinkPacket.hpp"
#include "MonotonicClock.hpp"
#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod_dsp.h>
#include <atomic>
#include <array>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <semaphore>
#include <memory>
using namespace geode::prelude;
namespace separate_song::audio_tap {
namespace {
struct Tap {
    static constexpr unsigned capacity = 64;
    std::array<EffectsPacketV2, capacity> queue;
    std::atomic<unsigned> read{0}, write{0};
    std::atomic<bool> enabled{true};
    uint32_t rate = 44100, sequence = 0, stream = 0;
    uint64_t clockStart = 0, clockFrames = 0;
    FMOD::DSP *dsp = nullptr;
    FMOD::ChannelGroup *group = nullptr;
    FMOD::System *system = nullptr;
    std::atomic<bool> stopping{false};
    std::counting_semaphore<capacity + 1> ready{0};
    std::thread worker;
    ~Tap() { shutdown(); }
    void shutdown() {
        enabled = false;
        // Remove the callback before freeing any queue or userdata storage.
        if (dsp) {
            // Exclude an in-flight render callback before retiring its userdata.
            bool locked = system && system->lockDSP() == FMOD_OK;
            auto engine = FMODAudioEngine::get();
            auto current = engine ? (stream == 1 ? engine->m_backgroundMusicChannel
                                                : engine->m_globalChannel) : nullptr;
            // removeDSP reconnects a live group's neighboring DSPs. Only fall
            // back to disconnecting the obsolete graph when its group changed.
            if (current && current == group)
                current->removeDSP(dsp);
            else
                dsp->disconnectAll(true, true);
            dsp->release();
            dsp = nullptr;
            if (locked)
                system->unlockDSP();
        }
        stopping = true;
        ready.release();
        if (worker.joinable())
            worker.join();
    }
    bool attachedTo(FMOD::ChannelGroup *current) const {
        if (!dsp || current != group)
            return false;
        int outputs = 0;
        return dsp->getNumOutputs(&outputs) == FMOD_OK && outputs > 0;
    }
    void push(float *input, unsigned frames, int channels) {
        if (!enabled.load(std::memory_order_relaxed) || channels < 1 || frames == 0)
            return;
        auto wall = monotonicNowNs();
        auto timestamp = clockStart + clockFrames * 1000000000 / rate;
        if (!clockStart || wall > timestamp + 20000000 || timestamp > wall + 100000000) {
            clockStart = wall;
            clockFrames = 0;
            timestamp = wall;
        }
        for (unsigned start = 0; start < frames; start += 512) {
            auto w = write.load(std::memory_order_relaxed);
            if (w - read.load(std::memory_order_acquire) >= capacity)
                break;
            auto &p = queue[w % capacity];
            p.frames = std::min(512u, frames - start);
            p.sequence = sequence++;
            p.sampleRate = rate;
            p.stream = stream;
            p.sessionID = processSessionID();
            p.timestamp = clockStart + (clockFrames + start) * 1000000000 / rate;
            for (unsigned i = 0; i < p.frames; ++i) {
                auto stereo = downmixStereo(input + (start + i) * channels, channels);
                p.samples[i * 2] = stereo[0];
                p.samples[i * 2 + 1] = stereo[1];
            }
            write.store(w + 1, std::memory_order_release);
            ready.release();
        }
        clockFrames += frames;
    }
    static FMOD_RESULT F_CALL render(FMOD_DSP_STATE *state, float *input, float *output, unsigned frames,
                                     int channels, int *outChannels) {
        *outChannels = channels;
        void *user = nullptr;
        state->functions->getuserdata(state, &user);
        if (user && input)
            static_cast<Tap *>(user)->push(input, frames, channels);
        if (input && output) {
            if (input != output)
                std::memcpy(output, input, frames * channels * sizeof(float));
        }
        return FMOD_OK;
    }
    bool attach(FMODAudioEngine *engine, FMOD::ChannelGroup *group, uint32_t id) {
        if (!group || !engine->m_system)
            return false;
        this->group = group;
        system = engine->m_system;
        stream = id;
        rate = engine->m_sampleRate > 0 ? engine->m_sampleRate : 44100;
        FMOD_DSP_DESCRIPTION description{};
        description.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
        std::strncpy(description.name,
                     id == 2 ? "OBS replacement"
                     : id    ? "OBS calibration"
                             : "OBS effects",
                     31);
        description.version = 0x10000;
        description.numinputbuffers = 1;
        description.numoutputbuffers = 1;
        description.read = render;
        description.userdata = this;
        if (engine->m_system->createDSP(&description, &dsp) != FMOD_OK)
            return false;
        int position = FMOD_CHANNELCONTROL_DSP_HEAD;
        if (id == 0) {
            FMOD::DSP *fader = nullptr;
            if (group->getDSP(FMOD_CHANNELCONTROL_DSP_FADER, &fader) != FMOD_OK ||
                group->getDSPIndex(fader, &position) != FMOD_OK) {
                dsp->release();
                dsp = nullptr;
                return false;
            }
            ++position;
        }
        if (group->addDSP(position, dsp) != FMOD_OK) {
            dsp->release();
            dsp = nullptr;
            return false;
        }
        worker = std::thread([this] {
            if (!socketsReady())
                return;
            auto socket = ::socket(AF_INET, SOCK_DGRAM, 0);
            if (socket == BAD_SOCKET)
                return;
            nonblocking(socket);
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(receiverPort());
            target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            for (;;) {
                ready.acquire();
                if (stopping.load(std::memory_order_acquire))
                    break;
                auto r = read.load(std::memory_order_relaxed);
                auto &p = queue[r % capacity];
                sendto(socket, reinterpret_cast<const char *>(&p), 44 + p.frames * 8, 0,
                       reinterpret_cast<sockaddr *>(&target), sizeof(target));
                read.store(r + 1, std::memory_order_release);
            }
            closeSocket(socket);
        });
        return true;
    }
};
// Explicit application lifecycle owns cleanup; never join from a DLL destructor.
Tap *effects = nullptr;
Tap *reference = nullptr;
bool attachFailureLogged = false;
bool captureEnabled = true;
bool terminated = false;
uint64_t nextAttach = 0;
unsigned retrySeconds = 1;
} // namespace
void install() {
    if (terminated)
        return;
    auto now = monotonicNowNs();
    if (now < nextAttach)
        return;
    auto engine = FMODAudioEngine::get();
    if (!engine || !engine->m_globalChannel)
        return;
    if (effects && effects->attachedTo(engine->m_globalChannel))
        return;
    delete effects;
    effects = nullptr;
    delete reference;
    reference = nullptr;
    auto tap = std::make_unique<Tap>();
    if (!tap->attach(engine, engine->m_globalChannel, 0)) {
        nextAttach = now + uint64_t(retrySeconds) * 1000000000;
        retrySeconds = std::min(retrySeconds * 2, 30u);
        if (!attachFailureLogged) {
            attachFailureLogged = true;
            log::error("OBS effects tap could not attach; retrying with backoff");
        }
        return;
    }
    effects = tap.release();
    effects->enabled = captureEnabled;
    attachFailureLogged = false;
    retrySeconds = 1;
    log::info("OBS effects tap attached at {} Hz", effects->rate);
#ifdef OBS_JUKEBOX_QA
    static const bool calibrate = std::getenv("SEPARATE_SONG_CALIBRATE") != nullptr;
    if (calibrate) {
        engine->setBackgroundMusicVolume(.5f);
        engine->setEffectsVolume(.5f);
        auto ref = std::make_unique<Tap>();
        if (ref->attach(engine, engine->m_backgroundMusicChannel, 1))
            reference = ref.release();
    }
#endif
}
void shutdown() {
    terminated = true;
    delete effects;
    effects = nullptr;
    delete reference;
    reference = nullptr;
}
void enable(bool value) {
    captureEnabled = value;
    if (effects)
        effects->enabled = value;
}
} // namespace separate_song::audio_tap
