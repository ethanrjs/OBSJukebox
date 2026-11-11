#include "../../src/Socket.hpp"
#include "../../src/LinkKey.hpp"
#include "../../src/LinkPacket.hpp"
#include "PitchShifter.hpp"
#include <obs.h>
#include <util/platform.h>
#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/resource.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Capture {
    std::mutex mutex;
    std::vector<float> samples;
    uint64_t first = 0, last = 0;
    size_t mutedFrames = 0;

    static void callback(void* param, obs_source_t*, audio_data const* a, bool muted) {
        auto& c = *static_cast<Capture*>(param);
        std::lock_guard lock(c.mutex);
        auto f = reinterpret_cast<float const*>(a->data[0]);
        if (f) c.samples.insert(c.samples.end(), f, f + a->frames);
        if (muted) c.mutedFrames += a->frames;
        if (!c.first) c.first = a->timestamp;
        c.last = a->timestamp;
    }

    void reset() {
        std::lock_guard lock(mutex);
        samples.clear();
        first = last = 0;
        mutedFrames = 0;
    }
};

struct Metric {
    size_t frames = 0;
    double rms = 0, hz = 0, cpu = 0, mutedFraction = 0, mean = 0;
    bool pass = false;
};

uint64_t cpuTicks() {
#ifdef _WIN32
    FILETIME a, b, k, u;
    GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
    ULARGE_INTEGER kk, uu;
    kk.LowPart = k.dwLowDateTime;
    kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime;
    uu.HighPart = u.dwHighDateTime;
    return kk.QuadPart + uu.QuadPart;
#else
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return uint64_t(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 10000000 +
        uint64_t(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 10;
#endif
}

// The group reference needs many frequencies, since a pure tone's shifted level depends on its
// starting phase. Filter the noise so fractional decoder interpolation barely changes its level;
// the quiet 440 Hz pilot still measures the output frequency.
struct PilotNoise {
    std::minstd_rand random{19};
    double filtered = 0;

    int16_t sample(uint64_t frame) {
        filtered += .05 * (double(random()) / random.max() * 2 - 1 - filtered);
        double const value =
            filtered * 39318 + 900 * std::sin(2 * 3.141592653589793 * 440 * double(frame) / 48000);
        return int16_t(std::clamp(value, -32767., 32767.));
    }
};

// A 440, 660, 880 then 1100 Hz tone, or with `constant` 440 Hz throughout.
void wave(
    std::filesystem::path const& path, double frequencyScale = 1, uint32_t samples = 48000 * 6,
    bool constant = false, bool pilotNoise = false
) {
    std::ofstream out(path, std::ios::binary);
    uint32_t size = samples * 4;
    auto put = [&](auto n) {
        out.write(reinterpret_cast<char const*>(&n), sizeof(n));
    };
    out.write("RIFF", 4);
    put(size + 36);
    out.write("WAVEfmt ", 8);
    put(uint32_t(16));
    put(uint16_t(1));
    put(uint16_t(2));
    put(uint32_t(48000));
    put(uint32_t(192000));
    put(uint16_t(4));
    put(uint16_t(16));
    out.write("data", 4);
    put(size);
    PilotNoise noise;
    for (uint32_t i = 0; i < samples; ++i) {
        double hz = (constant || i < 48000 ? 440 :
                         i < 96000         ? 660 :
                         i < 192000        ? 880 :
                                             1100) *
            frequencyScale;
        int16_t v = pilotNoise ? noise.sample(i) :
                                 int16_t(std::sin(2 * 3.141592653589793 * hz * i / 48000) * 9000);
        put(v);
        put(v);
    }
    out.close();
    std::error_code error;
    auto written = std::filesystem::file_size(path, error);
    if (!out || error || written != size + 44) {
        fprintf(stderr, "WAV fixture write failed: %s\n", path.string().c_str());
        std::exit(2);
    }
}

// Frame `frame` of the constant fixture as the decoder delivers it.
float toneSample(uint64_t frame) {
    return float(int16_t(std::sin(2 * 3.141592653589793 * 440 * double(frame) / 48000) * 9000)) /
        32768;
}

// The level the constant tone settles at through one fresh stage, from the plugin's own shifter fed
// 10 ms blocks as OBS feeds it. The stage cases compare OBS's output with this.
double stageRms(float pitch, uint16_t fft, bool pilotNoise = false, double fraction = 0) {
    PitchShifter stage;
    std::vector<float> block(960);
    double squares = 0;
    size_t counted = 0;
    PilotNoise noise;
    auto sample = [&](uint64_t frame) {
        return pilotNoise ? float(noise.sample(frame)) / 32768 : toneSample(frame);
    };
    float current = sample(0);
    for (uint64_t frame = 0; frame < 48000 * 2; frame += 480) {
        for (unsigned i = 0; i < 480; ++i) {
            float next = sample(frame + i + 1);
            block[i * 2] = block[i * 2 + 1] = float(current * (1 - fraction) + next * fraction);
            current = next;
        }
        stage.process(block.data(), 480, pitch, fft);
        if (frame >= 48000)
            for (unsigned i = 0; i < 480; ++i) {
                squares += double(block[i * 2]) * block[i * 2];
                ++counted;
            }
    }
    return std::sqrt(squares / double(counted));
}

bool validGroupFixture() {
    double const reference = stageRms(1.25f, 2048, true);
    for (double fraction : {.25, .5, .75}) {
        double const actual = stageRms(1.25f, 2048, true, fraction);
        if (std::abs(actual - reference) > reference * .05) {
            fprintf(
                stderr,
                "Group fixture interpolation %.2f changed RMS %.6f to %.6f beyond 5%%\n",
                fraction,
                reference,
                actual
            );
            return false;
        }
    }
    return true;
}

// The strongest frequency from half to twice `expected`, by a DFT over the whole capture. It
// measures a 4.4 Hz tone as well as 7 kHz, where counting zero crossings needs many cycles.
double dominantHz(std::vector<float> const& samples, double expected) {
    double best = 0, bestPower = -1;
    for (double hz = expected * .5; hz <= expected * 2; hz *= 1.002) {
        double const coefficient = 2 * std::cos(2 * 3.141592653589793 * hz / 48000);
        double s1 = 0, s2 = 0;
        for (float sample : samples) {
            double const s0 = sample + coefficient * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        double const power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
        if (power > bestPower) {
            bestPower = power;
            best = hz;
        }
    }
    return best;
}

int main(int argc, char** argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--check-group-fixture"))
        return validGroupFixture() ? 0 : 1;
    if (argc < 3) {
        fprintf(
            stderr, "Usage: harness plugin-path output-directory [optional-mp3 | --live [seconds]]\n"
        );
        return 2;
    }
    bool live = argc > 3 && !std::strcmp(argv[3], "--live");
    double liveSeconds = live && argc > 4 ? std::strtod(argv[4], nullptr) : 15;
    if (live && (!std::isfinite(liveSeconds) || liveSeconds < 1 || liveSeconds > 60)) {
        fprintf(stderr, "Live capture requires 1-60 seconds\n");
        return 2;
    }
    if (!live && !validGroupFixture()) return 2;

    if (!socketsReady()) {
        fprintf(stderr, "sockets unavailable\n");
        return 2;
    }
    auto preflight = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in preflightAddress{};
    preflightAddress.sin_family = AF_INET;
    preflightAddress.sin_port = htons(receiverPort());
    preflightAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (preflight == BAD_SOCKET ||
        bind(preflight, reinterpret_cast<sockaddr*>(&preflightAddress), sizeof(preflightAddress)) !=
            0) {
        if (preflight != BAD_SOCKET) closeSocket(preflight);
        fprintf(
            stderr, "UDP receiver port is already bound; refusing to compete with another receiver\n"
        );
        return 2;
    }
    closeSocket(preflight);
    std::filesystem::path output = std::filesystem::u8path(argv[2]);
    std::filesystem::create_directories(output);
    auto wav = output / std::filesystem::path(u8"runtime-曲-é.wav");
    if (!live) wave(wav);
    auto utf8 = wav.u8string();
    // Synthetic packets are signed with this process's key; live capture uses the key the real mod wrote.
    if (!live) {
        auto keyFile = (output / "link-key").string();
#ifdef _WIN32
        _putenv_s("OBS_JUKEBOX_TEST_KEY_FILE", keyFile.c_str());
#else
        setenv("OBS_JUKEBOX_TEST_KEY_FILE", keyFile.c_str(), 1);
#endif
        if (!writeLinkKey(processLinkKey(), keyFile)) {
            fprintf(stderr, "could not write the test link key\n");
            return 2;
        }
    }
    if (!obs_startup("en-US", nullptr, nullptr)) {
        fprintf(stderr, "obs_startup failed\n");
        return 2;
    }

#ifdef _WIN32
    auto const obsDirectory = std::getenv("OBS_QA_DIRECTORY");
    if (!obsDirectory || !*obsDirectory) {
        fprintf(
            stderr,
            "Set OBS_QA_DIRECTORY to the installed OBS directory (the PowerShell runner does "
            "this).\n"
        );
        obs_shutdown();
        return 2;
    }
    auto obsRoot =
        std::filesystem::path(std::u8string(reinterpret_cast<char8_t const*>(obsDirectory)));
    auto dataPath = (obsRoot / "data/libobs").generic_string() + "/";
    auto graphicsPath = (obsRoot / "bin/64bit/libobs-d3d11.dll").generic_string();
    obs_add_data_path(dataPath.c_str());
    char const* graphicsModule = graphicsPath.c_str();
#elif defined(__APPLE__)
    obs_add_data_path(OBS_QA_DATA_PATH);
    char const* graphicsModule = OBS_QA_GRAPHICS_MODULE;
#else
    char const* graphicsModule = "libobs-opengl.so";
#endif
    obs_video_info vi{};
    vi.graphics_module = graphicsModule;
    vi.fps_num = 30;
    vi.fps_den = 1;
    vi.base_width = vi.output_width = 128;
    vi.base_height = vi.output_height = 128;
    vi.output_format = VIDEO_FORMAT_RGBA;
    vi.colorspace = VIDEO_CS_709;
    vi.range = VIDEO_RANGE_FULL;
    vi.scale_type = OBS_SCALE_BILINEAR;
    if (obs_reset_video(&vi) != OBS_VIDEO_SUCCESS) {
        fprintf(stderr, "video tick initialization failed\n");
        obs_shutdown();
        return 2;
    }
    obs_audio_info ai{48000, SPEAKERS_STEREO};
    if (!obs_reset_audio(&ai)) {
        fprintf(stderr, "audio reset failed\n");
        return 2;
    }
    obs_module_t* module = nullptr;
    int opened = obs_open_module(&module, argv[1], argv[2]);
    if (opened != 0 || !obs_init_module(module)) {
        fprintf(stderr, "module failed %d\n", opened);
        return 2;
    }
    auto settings = obs_data_create();
    auto source =
        obs_source_create("gd_alternate_song", "Runtime validation GD Sounds", settings, nullptr);
    obs_data_release(settings);
    if (!source) {
        fprintf(stderr, "GD Sounds source failed\n");
        return 2;
    }
    Capture capture;
    obs_source_add_audio_capture_callback(source, Capture::callback, &capture);
    obs_source_inc_active(source);
    if (live) {
        printf(
            "LIVE capture: %.1f seconds of actual GD packets on port %d%s. No synthetic packets "
            "are sent.\n",
            liveSeconds,
            receiverPort(),
            receiverPort() == SONG_LINK_PORT ? "" : " (explicit relay port)"
        );
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::duration<double>(liveSeconds));
        obs_source_remove_audio_capture_callback(source, Capture::callback, &capture);
        double squares = 0, peak = 0;
        size_t nonzero = 0, invalid = 0;
        for (auto sample : capture.samples) {
            if (!std::isfinite(sample)) {
                ++invalid;
                continue;
            }
            squares += double(sample) * sample;
            peak = std::max(peak, double(std::abs(sample)));
            if (std::abs(sample) > 1e-5) ++nonzero;
        }
        auto frames = capture.samples.size();
        auto rms = frames ? std::sqrt(squares / frames) : 0;

        std::ofstream audio(output / "live-source-left.wav", std::ios::binary);
        auto put = [&](auto n) {
            audio.write(reinterpret_cast<char const*>(&n), sizeof(n));
        };
        uint32_t bytes = uint32_t(frames * sizeof(float));
        audio.write("RIFF", 4);
        put(bytes + 36);
        audio.write("WAVEfmt ", 8);
        put(uint32_t(16));
        put(uint16_t(3));
        put(uint16_t(1));
        put(uint32_t(48000));
        put(uint32_t(192000));
        put(uint16_t(4));
        put(uint16_t(32));
        audio.write("data", 4);
        put(bytes);
        audio.write(reinterpret_cast<char const*>(capture.samples.data()), bytes);
        audio.close();
        std::ofstream json(output / "live-source-results.json");
        json << "{\"scope\":\"Native OBS source left-channel PCM from live GD packets; no "
                "synthetic sender; an explicit relay port may strip music paths for effects-only "
                "validation; not physical listening\",\"receiver_port\":"
             << receiverPort() << ",\"observed_seconds\":" << liveSeconds
             << ",\"frames\":" << frames << ",\"nonzero_frames\":" << nonzero
             << ",\"invalid_samples\":" << invalid << ",\"rms\":" << rms << ",\"peak\":" << peak
             << ",\"muted_frames\":" << capture.mutedFrames
             << ",\"first_timestamp\":" << capture.first << ",\"last_timestamp\":" << capture.last
             << ",\"audio_present\":" << (nonzero ? "true" : "false") << "}\n";
        printf(
            "LIVE frames=%zu nonzero=%zu invalid=%zu rms=%.6f peak=%.6f muted=%zu\n",
            frames,
            nonzero,
            invalid,
            rms,
            peak,
            capture.mutedFrames
        );
        obs_source_dec_active(source);
        obs_source_release(source);
        obs_shutdown();
        return frames && invalid == 0 ? 0 : 1;
    }
    socketsReady();
    auto sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(receiverPort());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    nonblocking(sock);
    std::atomic<bool> finishClock{false};
    std::thread clockResponder([&] {
        while (!finishClock) {
            ClockSyncPacket reply{};
            sockaddr_in from{};
#ifdef _WIN32
            int length = sizeof(from);
#else
            socklen_t length = sizeof(from);
#endif
            auto size = recvfrom(
                sock,
                reinterpret_cast<char*>(&reply),
                sizeof(reply),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &length
            );
            if (size == sizeof(reply) && !memcmp(reply.magic, "GDCLK1\0", 8) && reply.kind == 1) {
                reply.kind = 2;
                reply.t2 = os_gettime_ns();
                reply.t3 = os_gettime_ns();
                signPacket(processLinkKey(), reply);
                sendto(
                    sock,
                    reinterpret_cast<char const*>(&reply),
                    sizeof(reply),
                    0,
                    reinterpret_cast<sockaddr*>(&from),
                    length
                );
            }
            else {
                waitSocketReadable(sock, 50);
            }
        }
    });
    SongLinkPacket packet;
    packet.sessionID = 0x51414e4154495644ULL;
    std::memcpy(packet.path, utf8.data(), std::min(utf8.size(), sizeof(packet.path) - 1));
    std::strcpy(packet.song, "Runtime synthetic tone");
    std::strcpy(packet.status, "playing");
    packet.flags = 3;
    packet.epoch = 1;
    // The mod's global packet: effects, their volume and the group stages. It goes out with each
    // voice packet, on the same session, socket and timestamp.
    SongLinkPacket global;
    global.channelID = -1;
    // With `spectral` a case measures its frequency by DFT; with `expectedRms` its level must be
    // within 5% of that oracle. Both stay set until changed, like the packets' volumes.
    bool spectral = false;
    double expectedRms = 0;
    std::ofstream json(output / "runtime-results.json");
    json << "{\"host\":\"Installed OBS libobs\",\"plugin\":\""
         << std::filesystem::path(argv[1]).filename().string() << "\",\"cases\":[\n";
    bool first = true;
    int failures = 0;
    // effects: 1 sends a 0.15 level each tick, 2 the tap's 44.1 kHz bursts of that level, 3 the
    // constant tone at 48 kHz, 30 ms ahead so a coarse timer cannot make a staged block late.
    // A case sends packets for `settle` + `measure` seconds and measures only the last `measure`:
    // the settle is the time the previous state needs to leave the speakers and this one to reach
    // them (the decoder renders two 10 ms blocks ahead, and a changed epoch seeks once), and the
    // measured window only has to be long enough for its check. 120 ms is 53 cycles at 440 Hz, far
    // inside the 8% frequency tolerance, and 26 at 220 Hz; the silence, level and mute checks need
    // a fraction of that.
    double const settle = .08, measure = .12;
    auto phase = [&](char const* name,
                     double settleSeconds,
                     double measureSeconds,
                     double expected,
                     double pos,
                     uint32_t flags,
                     double rate = 1,
                     double offset = 0,
                     bool transmit = true,
                     int effects = 0,
                     bool expectedMuted = false,
                     bool expectEffectMean = false,
                     bool advanceEpoch = true) {
        if (std::getenv("OBS_QA_RECOVERY_ONLY") && std::strcmp(name, "mp3_decode") &&
            std::strcmp(name, "failed_seek_music_silence") &&
            std::strcmp(name, "failed_seek_effects_continue") &&
            std::strcmp(name, "failed_seek_restart_recovery"))
            return Metric{};
        packet.position = pos;
        packet.flags = flags;
        packet.rate = rate;
        packet.offset = offset;
        if (advanceEpoch) packet.epoch++;
        double const seconds = settleSeconds + measureSeconds;
        auto start = Clock::now(),
             end = start +
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
        uint64_t cpu = cpuTicks();
        capture.reset();
        bool reset = false;
        uint64_t effectClock = os_gettime_ns(), effectFrames = 0;
        uint32_t sequence = 0;
        while (Clock::now() < end) {
            double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
            packet.position = pos + ((flags & 2) ? elapsed * rate : 0);
            packet.timestamp = os_gettime_ns();
            global.sessionID = packet.sessionID;
            global.timestamp = packet.timestamp;
            global.flags = flags & 1;
            global.musicVolume = packet.musicVolume;
            global.effectsVolume = packet.effectsVolume;
            if (transmit) {
                signPacket(processLinkKey(), global);
                signPacket(processLinkKey(), packet);
                sendto(
                    sock,
                    reinterpret_cast<char const*>(&global),
                    sizeof(global),
                    0,
                    reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)
                );
                sendto(
                    sock,
                    reinterpret_cast<char const*>(&packet),
                    sizeof(packet),
                    0,
                    reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)
                );
            }
            auto sendEffects = [&](EffectsPacketV3& e) {
                auto size = offsetof(EffectsPacketV3, samples) + e.frames * 8;
                signPacket(processLinkKey(), e, size);
                sendto(
                    sock,
                    reinterpret_cast<char const*>(&e),
                    int(size),
                    0,
                    reinterpret_cast<sockaddr*>(&addr),
                    sizeof(addr)
                );
            };
            if (effects == 1) {
                EffectsPacketV3 e;
                e.sessionID = packet.sessionID;
                e.frames = 480;
                e.sampleRate = 48000;
                e.timestamp = os_gettime_ns();
                for (unsigned i = 0; i < e.frames * 2; ++i)
                    e.samples[i] = .15f;
                sendEffects(e);
            }
            if (effects == 2) {
                double jitter = (effectFrames / 1024) % 2 ? .006 : 0;
                while (elapsed >= double(effectFrames) / 44100 + jitter) {
                    for (unsigned part = 0; part < 2; ++part) {
                        EffectsPacketV3 e;
                        e.sessionID = packet.sessionID;
                        e.frames = 512;
                        e.sampleRate = 44100;
                        e.sequence = sequence++;
                        e.timestamp = effectClock + effectFrames * 1000000000 / 44100;
                        effectFrames += 512;
                        for (unsigned i = 0; i < e.frames * 2; ++i)
                            e.samples[i] = .15f;
                        sendEffects(e);
                    }
                    jitter = (effectFrames / 1024) % 2 ? .006 : 0;
                }
            }
            while (effects == 3 && elapsed + .03 >= double(effectFrames) / 48000) {
                EffectsPacketV3 e;
                e.sessionID = packet.sessionID;
                e.frames = 480;
                e.sampleRate = 48000;
                e.sequence = sequence++;
                e.timestamp = effectClock + effectFrames * 1000000000 / 48000;
                for (unsigned i = 0; i < e.frames; ++i)
                    e.samples[i * 2] = e.samples[i * 2 + 1] = toneSample(effectFrames + i);
                effectFrames += 480;
                sendEffects(e);
            }
            if (!reset && elapsed > settleSeconds) {
                capture.reset();
                reset = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        double cpuPercent = double(cpuTicks() - cpu) / 1e7 / seconds * 100;
        Metric m;
        {
            std::lock_guard lock(capture.mutex);
            m.frames = capture.samples.size();
            double squares = 0, total = 0;
            size_t crosses = 0;
            for (size_t i = 0; i < m.frames; ++i) {
                double v = capture.samples[i];
                squares += v * v;
                total += v;
                if (i && capture.samples[i - 1] <= 0 && v > 0) crosses++;
            }
            if (m.frames) {
                m.rms = std::sqrt(squares / m.frames);
                m.mean = total / m.frames;
                m.hz = double(crosses) * 48000 / m.frames;
                m.mutedFraction = double(capture.mutedFrames) / m.frames;
            }
            if (spectral && expected > 0) m.hz = dominantHz(capture.samples, expected);
        }
        m.pass = m.frames > 1000 &&
            (expected == 0    ? m.rms < .00001 :
                 expected < 0 ? m.rms > .05 :
                                m.rms > .05 && std::abs(m.hz - expected) < expected * .08) &&
            (expectedMuted ? m.mutedFraction > .99 : m.mutedFraction < .01) &&
            (!expectEffectMean || m.mean > .03);
        if (expectedRms > 0 && std::abs(m.rms - expectedRms) > expectedRms * .05) m.pass = false;
        if (effects == 2) {
            std::lock_guard lock(capture.mutex);
            for (float sample : capture.samples)
                if (std::abs(sample - .15f * packet.effectsVolume) > .001f) m.pass = false;
        }
        if (packet.musicVolume == .5f && expected > 0 && !effects)
            m.pass = m.pass && m.rms > .09 && m.rms < .105;
        if (!m.pass) failures++;
        if (!first) json << ",\n";
        first = false;
        json << "{\"name\":\"" << name << "\",\"frames\":" << m.frames << ",\"raw_rms\":" << m.rms
             << ",\"sample_mean\":" << m.mean << ",\"frequency_hz\":" << m.hz
             << ",\"frequency_method\":\"" << (spectral && expected > 0 ? "dft" : "zero_crossings")
             << "\",\"expected_rms\":" << expectedRms
             << ",\"obs_callback_muted_fraction\":" << m.mutedFraction
             << ",\"process_cpu_one_core_percent\":" << cpuPercent
             << ",\"pass\":" << (m.pass ? "true" : "false") << "}";
        printf(
            "%s: %s rms=%.5f hz=%.1f mean=%.4f muted=%.2f CPU=%.2f%% of one core\n",
            name,
            m.pass ? "PASS" : "FAIL",
            m.rms,
            m.hz,
            m.mean,
            m.mutedFraction,
            cpuPercent
        );
        fflush(stdout);
        return m;
    };
    phase("no_link_silence", settle, measure, 0, 0, 0, 1, 0, false);
    // The first case to open a file gets longer, in case the first read of a fresh fixture is slow.
    phase("unicode_wav_decode", .2, measure, 440, 0, 3);
    packet.musicVolume = 0;
    phase(
        "active_volume_mute_same_path_and_epoch", settle, measure, 0, 1, 3, 1, 0, true, 0, false, false, false
    );
    packet.musicVolume = 1;
    phase(
        "active_volume_restore_same_path_and_epoch", settle, measure, 880, 2, 3, 1, 0, true, 0, false, false, false
    );
    packet.musicVolume = 0;
    phase("obs_music_muted", settle, measure, 0, 0, 3);
    phase("obs_music_muted_effects_unaffected", settle, measure, -1, 0, 3, 1, 0, true, 2);
    packet.musicVolume = .5f;
    phase("obs_music_half_gain", settle, measure, 440, 0, 3);
    packet.musicVolume = 1;
    packet.effectsVolume = 0;
    phase("obs_effects_muted", settle, measure, 0, 0, 1, 1, 0, true, 2);
    phase("obs_effects_muted_music_unaffected", settle, measure, 440, 0, 3, 1, 0, true, 1);
    packet.effectsVolume = .5f;
    phase("obs_effects_half_gain", settle, measure, -1, 0, 1, 1, 0, true, 2);
    packet.effectsVolume = 1;
    phase("pause_silence", settle, measure, 0, .3, 1);
    phase("resume", settle, measure, 440, 0, 3);
    phase("pause_before_song_switch", settle, measure, 0, 2, 1, 1, 0, true, 0, false, false, false);
    auto alternate = output / "paused-selection.wav";
    wave(alternate, .5);
    auto alternateUtf8 = alternate.u8string();
    std::memset(packet.path, 0, sizeof(packet.path));
    std::memcpy(
        packet.path, alternateUtf8.data(), std::min(alternateUtf8.size(), sizeof(packet.path) - 1)
    );
    phase("song_path_switch_while_paused", settle, measure, 0, 2, 1, 1, 0, true, 0, false, false, false);
    phase(
        "switched_song_resume_at_current_position", settle, measure, 440, 2, 3, 1, 0, true, 0, false, false, false
    );
    std::memset(packet.path, 0, sizeof(packet.path));
    phase(
        "temporary_empty_path_while_paused", settle, measure, 0, 2, 1, 1, 0, true, 0, false, false, false
    );
    std::memcpy(
        packet.path, alternateUtf8.data(), std::min(alternateUtf8.size(), sizeof(packet.path) - 1)
    );
    phase("restored_path_remains_paused", settle, measure, 0, 2, 1, 1, 0, true, 0, false, false, false);
    phase(
        "restored_path_resumes_at_current_position", settle, measure, 440, 2, 3, 1, 0, true, 0, false, false, false
    );
    std::memset(packet.path, 0, sizeof(packet.path));
    std::memcpy(packet.path, utf8.data(), std::min(utf8.size(), sizeof(packet.path) - 1));
    phase("death_stop_silence", settle, measure, 0, 1.2, 1);
    phase("epoch_reset_seek", settle, measure, 880, 2, 3);
    phase("rate_half", settle, measure, 220, 0, 3, .5);
    phase("positive_offset_seek", settle, measure, 880, 0, 3, 1, 2);
    phase("negative_offset_silence", settle, measure, 0, 0, 3, 1, -2);
    phase("effects_qpc_timestamp", settle, measure, -1, 0, 1, 1, 0, true, true);
    // The tap's burst generator starts its timeline with the case, so the first blocks after the
    // switch from the 48 kHz path need longer than the default settle.
    phase("effects_fmod_44100_burst_continuity", .3, .2, -1, 0, 1, 1, 0, true, 2);
    std::strcpy(packet.status, "Complete");
    phase("music_continues_on_completion_screen", settle, measure, 880, 2, 3);
    phase("completed_music_stops_at_file_end", settle, measure, 0, 6.2, 3);
    phase("effects_continue_after_level_complete", settle, measure, -1, 0, 1, 1, 0, true, 2);
    std::strcpy(packet.status, "playing");
    // The effects add a constant offset to the music, so the zero-crossing count is noisy near zero
    // and this case measures its frequency by DFT.
    spectral = true;
    phase("combined_music_and_effects", settle, measure, 440, 0, 3, 1, 0, true, true, false, true);
    spectral = false;
    phase(
        "extreme_offset_music_silent_effects_play", settle, measure, -1, 0, 3, 1, 1e9, true, true, false, true
    );
    obs_source_set_muted(source, true);
    phase("obs_source_mute_flag", settle, measure, 440, 0, 3, 1, 0, true, false, true);
    obs_source_set_muted(source, false);
    obs_source_dec_active(source);
    phase("inactive_music_and_effects_silence", settle, measure, 0, 0, 3, 1, 0, true, true);
    obs_source_inc_active(source);
    // The last packet keeps its voice live for 400 ms after its timestamp, which is the stretch that
    // covers a gap in the game's packets, and the voice then fades out, so 600 ms leaves 200 ms of
    // margin before the measured window starts.
    phase("link_timeout_silence", .6, measure, 0, 0, 3, 1, 0, false);
    auto delayed = output / "delayed-download.wav";
    std::filesystem::remove(delayed);
    auto delayedUtf8 = delayed.u8string();
    std::memset(packet.path, 0, sizeof(packet.path));
    std::memcpy(packet.path, delayedUtf8.data(), std::min(delayedUtf8.size(), sizeof(packet.path) - 1));
    phase("missing_song_silence", settle, measure, 0, 0, 3);
    wave(delayed);
    // The file appears at the same path and epoch, so only the plugin's reopen can pick it up, and
    // that runs at most once a second after the failed attempt: a retry can fire up to 1 s into this
    // case, and 1.2 s of settle leaves 200 ms past it.
    phase(
        "missing_song_same_path_and_epoch_recovery", 1.2, .2, -1, 0, 3, 1, 0, true, 0, false, false, false
    );
    auto replaced = output / "incomplete-download.wav";
    {
        std::ofstream incomplete(replaced, std::ios::binary);
        incomplete << "incomplete download";
    }
    auto replacedUtf8 = replaced.u8string();
    std::memset(packet.path, 0, sizeof(packet.path));
    std::memcpy(
        packet.path, replacedUtf8.data(), std::min(replacedUtf8.size(), sizeof(packet.path) - 1)
    );
    // This case's own failed open schedules the reopen one second in, and the window has to straddle
    // it: it starts 230 ms before the retry and ends 370 ms after.
    phase("invalid_song_retry_effects_continuity", .8, .6, -1, 0, 3, 1, 0, true, 2);
    wave(replaced);
    // The same reopen as above: it can fire up to 1 s into this case, and 1.2 s leaves 200 ms past it.
    phase(
        "invalid_song_same_path_and_epoch_recovery", 1.2, .2, -1, 0, 3, 1, 0, true, 0, false, false, false
    );
#ifndef _WIN32
    auto growing = output / "growing-download.wav";
    wave(growing);
    std::filesystem::resize_file(growing, 44 + 4800 * 4);
    auto growingUtf8 = growing.u8string();
    std::memset(packet.path, 0, sizeof(packet.path));
    std::memcpy(packet.path, growingUtf8.data(), std::min(growingUtf8.size(), sizeof(packet.path) - 1));
    phase("partial_valid_song_exhausted", settle, measure, 0, 2, 3);
    wave(growing);
    // The same reopen as above: it can fire up to 1 s into this case, and 1.2 s leaves 200 ms past it.
    phase(
        "growing_song_same_path_and_epoch_recovery", 1.2, .2, 880, 1, 3, 1, 0, true, 0, false, false, false
    );
#endif
    if (argc > 3) {
        std::strncpy(packet.path, argv[3], sizeof(packet.path) - 1);
        phase("mp3_decode", settle, measure, -1, 0, 3);
        phase("failed_seek_music_silence", settle, measure, 0, 100, 3);
        phase(
            "failed_seek_effects_continue", settle, measure, -1, 100, 3, 1, 0, true, true, false, true
        );
        phase("failed_seek_restart_recovery", settle, measure, -1, 0, 3);
    }
    std::memset(packet.path, 0, sizeof(packet.path));
    std::memcpy(packet.path, utf8.data(), std::min(utf8.size(), sizeof(packet.path) - 1));
    // v7 speed and pitch stages on a constant 440 Hz tone. The file lasts 32 s, so 16x still has
    // audio after the settle. Each case clears its stages, and the previous path comes back after.
    {
        auto constant = output / "constant-440.wav";
        wave(constant, 1, 48000 * 32, true);
        char previous[sizeof(packet.path)];
        std::memcpy(previous, packet.path, sizeof(previous));
        auto constantUtf8 = constant.u8string();
        std::memset(packet.path, 0, sizeof(packet.path));
        std::memcpy(
            packet.path, constantUtf8.data(), std::min(constantUtf8.size(), sizeof(packet.path) - 1)
        );
        spectral = true;
        // 0.01x of 4.4 kHz is what 0.01x of the 440 Hz fixture would be at 4.4 Hz: the same one
        // constant slow tone, with ten times the cycles in the window the DFT needs to find it.
        auto slowTone = output / "constant-4400.wav";
        wave(slowTone, 10, 48000 * 4, true);
        auto slowUtf8 = slowTone.u8string();
        std::memset(packet.path, 0, sizeof(packet.path));
        std::memcpy(packet.path, slowUtf8.data(), std::min(slowUtf8.size(), sizeof(packet.path) - 1));
        phase("rate_hundredth_constant_tone", .25, .6, 44, 0, 3, .01);
        std::memset(packet.path, 0, sizeof(packet.path));
        std::memcpy(
            packet.path, constantUtf8.data(), std::min(constantUtf8.size(), sizeof(packet.path) - 1)
        );
        phase("rate_16x_constant_tone", .25, .4, 7040, 0, 3, 16);
        packet.stageCount = 1;
        packet.stages[0] = {.75f, 4096, 0, 0x7001};
        expectedRms = stageRms(.75f, 4096);
        // A stage holds the audio for its FFT size, 64 ms at 4096, before it outputs anything, so
        // these cases settle longer than the rest.
        phase("channel_stage_three_quarters", .18, .17, 330, 2, 3);
        // The same stage at a new pitch keeps its instance, so OBS keeps the stage and its audio. Its
        // level then depends on the phase it carried over, which no fresh stage reproduces: it has
        // measured .026 against .19 for a whole window, so this level only proves the stage is not
        // silent and the pitch is what this case is for. The chain tests cover what the stage keeps.
        packet.stages[0].pitch = 1.5f;
        expectedRms = 0;
        phase(
            "channel_stage_same_instance_pitch_change", .55, 1.45, 660, 4, 3, 1, 0, true, 0, false, false, false
        );
        packet.stages[0] = {1.f, 4096, 0, 0x7002};
        expectedRms = stageRms(1.f, 4096);
        phase("channel_stage_pitch_one_gain", .18, .17, 440, 2, 3);
        packet.stageCount = 0;
        packet.stages[0] = {};
        auto groupTone = output / "group-pilot-noise.wav";
        wave(groupTone, 1, 48000 * 6, true, true);
        auto groupUtf8 = groupTone.u8string();
        std::memset(packet.path, 0, sizeof(packet.path));
        std::memcpy(packet.path, groupUtf8.data(), std::min(groupUtf8.size(), sizeof(packet.path) - 1));
        global.stageCount = 1;
        global.stages[0] = {1.25f, 2048, 0, 0x7003};
        expectedRms = stageRms(1.25f, 2048, true);
        // The 440 Hz pilot sits 33 dB below the noise and only just above the noise's band, so the
        // DFT needs a long window to pick it out.
        phase("group_stage_shifts_music", .18, 1.0, 550, 0, 3);
        std::memset(packet.path, 0, sizeof(packet.path));
        std::memcpy(
            packet.path, constantUtf8.data(), std::min(constantUtf8.size(), sizeof(packet.path) - 1)
        );
        expectedRms = 0;
        phase("excluded_replacement_skips_group_stage", .18, .17, 440, 2, 3 | 8);
        global.stageCount = 0;
        global.stages[0] = {};
        global.effectsStageCount = 1;
        global.effectsStages[0] = {.5f, 1024, 0, 0x7004};
        expectedRms = stageRms(.5f, 1024);
        phase("effects_stage_shifts_effects", .18, .17, 220, 0, 1, 1, 0, true, 3);
        global.effectsStageCount = 0;
        global.effectsStages[0] = {};
        expectedRms = 0;
        spectral = false;
        std::memcpy(packet.path, previous, sizeof(previous));
    }
    // A different session can take ownership only after 2 s without a packet from the previous one,
    // so this case has to stay quiet for longer than that: its length is the wait, not a measurement,
    // and the 200 ms past the timeout keeps a slow handoff from failing on a busy runner.
    phase("session_handoff_quiet_window", settle, 2.12, 0, 0, 0, 1, 0, false);
    packet.sessionID = 0x51414e4154495645ULL;
    // The first packet adopts the new session and one clock probe and reply make its clock usable,
    // which the longer settle covers.
    phase("v5_clock_synchronized_playback", .3, .2, 440, 0, 3);
    packet.triggerGain = 0;
    phase("v5_trigger_volume_zero", settle, measure, 0, 0, 3);
    packet.triggerGain = .5f;
    packet.musicVolume = 1;
    auto gainResult = phase("v5_trigger_and_obs_independent_gain", settle, measure, -1, 0, 3);
    if (gainResult.rms < .09 || gainResult.rms > .105) ++failures;
    packet.triggerGain = 1;
    packet.musicVolume = 1;
    phase("v5_effects_share_clock_mapping", settle, measure, -1, 0, 1, 1, 0, true, 1);
    finishClock = true;
    clockResponder.join();
    json << "\n],\"failures\":"
         << failures << ",\"scope\":\"Native source PCM callbacks; synthetic protocol; not physical audio or live Geometry Dash\"}\n";
    json.close();
    closeSocket(sock);
    obs_source_remove_audio_capture_callback(source, Capture::callback, &capture);
    obs_source_dec_active(source);
    obs_source_release(source);
    obs_shutdown();
    return failures ? 1 : 0;
}
