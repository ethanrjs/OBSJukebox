// Measures what the plugin's audio costs and how close its pitch stage stays to GD's FMOD. It
// reports numbers and fails nothing: playback-tests.cpp holds the pass/fail checks. Build and run it
// on an otherwise idle machine with `scripts/tests/audio/run.ps1 -Bench <mode>` or
// `scripts/tests/audio/run.sh --bench <mode>`.
//
//   bench pitch      CPU per second of audio for one stage, shipping and 41b8647 runs interleaved
//   bench render     each step of the plugin's pipeline on simulated time, one thread, per block
//   bench threads    the real output thread and workers under load, per thread, for misses and lateness
//   bench link       signing and checking packets, on the mod's side and the plugin's
//   bench fidelity   the shipping stage against the 41b8647 reference and FMOD's recordings
//   bench open       opening, seeking and the first block of the song files given, in wall time
#define main playbackTests
#include "playback-tests.cpp"
#undef main
#include <cstdlib>
#include <cstring>
#include <map>
#include <numeric>
#ifdef _WIN32
#include <processthreadsapi.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#ifdef __APPLE__
#include <mach/mach.h>
#endif

// The receiver's packet handling and thread, which only its friends can reach.
struct BenchAccess {
    static void handle(Receiver &r, const void *data, size_t size, uint16_t port, uint64_t now) {
        sockaddr_in from{};
        from.sin_family = AF_INET;
        from.sin_port = htons(port);
        from.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        r.handle(static_cast<const unsigned char *>(data), size, from, now);
    }
    static std::thread &worker(Receiver &r) { return r.worker; }
};

namespace bench {
using Ns = std::chrono::nanoseconds;
static uint64_t wallNs() { return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()); }

// CPU time a thread has used. Windows counts CPU cycles; they are converted with a rate measured once
// against the wall clock while this thread spins, so its figures are cycle-equivalent nanoseconds.
#ifdef _WIN32
static double cyclesPerNs() {
    static const double rate = [] {
        ULONG64 begin = 0, end = 0;
        QueryThreadCycleTime(GetCurrentThread(), &begin);
        const auto start = wallNs();
        while (wallNs() - start < 300000000) {
        }
        QueryThreadCycleTime(GetCurrentThread(), &end);
        return double(end - begin) / double(wallNs() - start);
    }();
    return rate;
}
static uint64_t cpuNs(HANDLE thread) {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(thread, &cycles);
    return uint64_t(double(cycles) / cyclesPerNs());
}
static uint64_t cpuNs(std::thread &thread) { return cpuNs(HANDLE(thread.native_handle())); }
static uint64_t cpuNs() { return cpuNs(GetCurrentThread()); }
#elif defined(__APPLE__)
static uint64_t cpuNs(pthread_t thread) {
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(thread), THREAD_BASIC_INFO, thread_info_t(&info), &count) != KERN_SUCCESS)
        return 0;
    return (uint64_t(info.user_time.seconds) + uint64_t(info.system_time.seconds)) * 1000000000ULL +
           (uint64_t(info.user_time.microseconds) + uint64_t(info.system_time.microseconds)) * 1000ULL;
}
#else
static uint64_t cpuNs(pthread_t thread) {
    clockid_t clock;
    timespec t{};
    if (pthread_getcpuclockid(thread, &clock) || clock_gettime(clock, &t))
        return 0;
    return uint64_t(t.tv_sec) * 1000000000ULL + uint64_t(t.tv_nsec);
}
#endif
#ifndef _WIN32
static uint64_t cpuNs(std::thread &thread) { return cpuNs(thread.native_handle()); }
static uint64_t cpuNs() { return cpuNs(pthread_self()); }
#endif

struct Samples {
    std::vector<double> values;
    void add(double value) { values.push_back(value); }
    double at(double fraction) {
        if (values.empty())
            return 0;
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, size_t(fraction * double(values.size())))];
    }
    double total() const { return std::accumulate(values.begin(), values.end(), 0.); }
    // p50 / p95 / p99 / max, in the unit the values were added in.
    std::string spread() {
        char text[96];
        std::snprintf(text, sizeof(text), "%8.1f %8.1f %8.1f %8.1f", at(.5), at(.95), at(.99), at(1));
        return text;
    }
};

// One stage over ten seconds of stereo noise in OBS's 480-frame calls, shipping and reference runs
// interleaved, taking turns to go first, so drift in clock speed hits both. Reports CPU ms per second
// of audio (median of the rounds) and per-call microseconds.
template <class Shifter> static double timeStage(const heldout::Signal &signal, float pitch, uint32_t fft, Samples &calls) {
    auto audio = signal;
    Shifter shifter(480);
    calls.values.reserve(calls.values.size() + audio.size() / 2 / 480);
    const auto cpuBegin = cpuNs();
    for (size_t at = 0; at < audio.size() / 2; at += 480) {
        const auto begin = wallNs();
        shifter.process(audio.data() + at * 2, 480, pitch, fft);
        calls.add(double(wallNs() - begin) / 1000);
    }
    return double(cpuNs() - cpuBegin) / 1e6 / (double(audio.size() / 2) / 48000);
}
static void pitch(unsigned rounds) {
    std::cout << "pitch stage, CPU ms per second of stereo audio (median of " << rounds
              << " rounds) and per-call us p50/p95/p99/max\n";
    for (bool identical : {false, true}) {
        const auto signal = identical ? heldout::mono(480000) : heldout::noise(480000, 21);
        for (uint32_t fft : {256u, 512u, 1024u, 2048u, 4096u})
            for (float pitch : {.5f, .75f, 1.f, 1.5f, 2.f}) {
                Samples shipping, reference, shippingCalls, referenceCalls, warmup;
                timeStage<reference_41b8647::PitchShifter>(signal, pitch, fft, warmup);
                timeStage<PitchShifter>(signal, pitch, fft, warmup);
                for (unsigned round = 0; round < rounds; ++round) {
                    auto runReference = [&] {
                        reference.add(timeStage<reference_41b8647::PitchShifter>(signal, pitch, fft, referenceCalls));
                    };
                    auto runShipping = [&] { shipping.add(timeStage<PitchShifter>(signal, pitch, fft, shippingCalls)); };
                    if (round % 2)
                        runShipping(), runReference();
                    else
                        runReference(), runShipping();
                }
                std::printf("%s fft %4u pitch %.2f | 41b8647 %6.2f ms/s (%.2f-%.2f) %s | shipping %6.2f ms/s "
                            "(%.2f-%.2f) %s | %+.1f%%\n",
                            identical ? "mono  " : "stereo", fft, pitch, reference.at(.5), reference.at(0),
                            reference.at(1), referenceCalls.spread().c_str(), shipping.at(.5), shipping.at(0),
                            shipping.at(1), shippingCalls.spread().c_str(),
                            100 * (shipping.at(.5) / reference.at(.5) - 1));
            }
    }
}

// A 30-second stereo WAV whose channels differ, at `rate`.
static void writeStereoWav(const char *path, uint32_t rate) {
    std::ofstream out(path, std::ios::binary);
    const uint32_t frames = rate * 30, bytes = frames * 4;
    auto word = [&](uint32_t value, int count) {
        for (int i = 0; i < count; ++i)
            out.put(char(value >> (i * 8)));
    };
    out.write("RIFF", 4); word(36 + bytes, 4); out.write("WAVEfmt ", 8); word(16, 4);
    word(1, 2); word(2, 2); word(rate, 4); word(rate * 4, 4); word(4, 2); word(16, 2);
    out.write("data", 4); word(bytes, 4);
    for (uint32_t i = 0; i < frames; ++i) {
        const double t = double(i) / rate;
        word(uint16_t(int16_t(8000 * std::sin(heldout::tau * 220 * t) + 3000 * std::sin(heldout::tau * 1760 * t))), 2);
        word(uint16_t(int16_t(7000 * std::sin(heldout::tau * 330 * t) + 2000 * std::sin(heldout::tau * 2637 * t))), 2);
    }
}

struct Workload {
    const char *name;
    unsigned voices = 0;
    const char *path = "artifacts/audio-tests/long.mp3";
    unsigned voiceStages = 0;     // 4096 stages on each voice, at most 8
    bool groupStage = false;      // one 4096 stage on the music group
    bool effects = false;         // the game sends effects
    bool monoEffects = true;      // centred SFX: identical channels
    unsigned effectsStages = 0;   // 4096 stages after the tap
    uint32_t effectsRate = 44100; // GD's tap sends 512 frames at the game's output rate
    float pitch = .85f;
    uint16_t fft = 4096;
    bool stress = false; // the deadline tests' worst case: four voices with one stage, one with six
};
static const std::vector<Workload> &workloads() {
    static const std::vector<Workload> list{
        {.name = "idle (source active, no game)"},
        {.name = "1 voice 48 kHz WAV", .voices = 1, .path = "artifacts/audio-tests/bench-48k.wav"},
        {.name = "1 voice 44.1 kHz WAV", .voices = 1, .path = "artifacts/audio-tests/bench-44k.wav"},
        {.name = "1 voice MP3", .voices = 1},
        {.name = "5 voices MP3", .voices = 5},
        {.name = "effects only", .effects = true},
        {.name = "1 voice MP3 + effects", .voices = 1, .effects = true},
        {.name = "1 voice, channel stage 1024 x0.75", .voices = 1, .voiceStages = 1, .pitch = .75f, .fft = 1024},
        {.name = "1 voice, channel stage 4096", .voices = 1, .voiceStages = 1},
        {.name = "1 voice, group stage 4096", .voices = 1, .groupStage = true},
        {.name = "1 voice, 3 chained stages 2048", .voices = 1, .voiceStages = 3, .fft = 2048},
        {.name = "effects, 1 stage 4096 (centred SFX)", .effects = true, .effectsStages = 1},
        {.name = "effects, 1 stage 4096 (stereo SFX)", .effects = true, .monoEffects = false, .effectsStages = 1},
        {.name = "effects 48 kHz, 1 stage 4096", .effects = true, .effectsStages = 1, .effectsRate = 48000},
        {.name = "stress: 5 voices, 10+1+8 stages", .voices = 5, .groupStage = true, .effects = true,
         .monoEffects = false, .effectsStages = 8, .stress = true},
    };
    return list;
}
// The packets a game playing `w` sends; positions follow simulated time.
struct Game {
    Workload w;
    SongLinkPacket global;
    std::vector<SongLinkPacket> voices;
    uint64_t start = 0, sentFrames = 0;
    unsigned sequence = 0;
    explicit Game(const Workload &workload, uint64_t now) : w(workload), start(now) {
        global.channelID = -1;
        global.flags = w.effects ? 1 : 0;
        if (w.groupStage) {
            global.stageCount = 1;
            global.stages[0] = {.9f, 4096, 0, 900};
        }
        global.effectsStageCount = uint8_t(w.effectsStages);
        for (unsigned i = 0; i < w.effectsStages; ++i)
            global.effectsStages[i] = {.8f + float(i) * .02f, 4096, 0, 910 + i};
        voices.resize(w.voices);
        for (unsigned v = 0; v < w.voices; ++v) {
            auto &p = voices[v];
            p.flags = 3;
            p.channelID = int(v);
            p.epoch = 1;
            std::strcpy(p.path, w.path);
            p.stageCount = uint8_t(w.stress ? (v < 4 ? 1 : 6) : w.voiceStages);
            for (unsigned s = 0; s < p.stageCount; ++s)
                p.stages[s] = {w.stress ? .85f + float(v) * .02f + float(s) * .01f : w.pitch + float(s) * .05f,
                               w.fft, 0, 920 + v * 8 + s};
        }
    }
    std::vector<SongLinkPacket> songPackets(uint64_t now) {
        std::vector<SongLinkPacket> packets{global};
        for (auto &p : voices) {
            p.position = 1 + double(now - start) / 1e9;
            packets.push_back(p);
        }
        return packets;
    }
    // Effects packets whose 512 frames started by `now`.
    std::vector<EffectsPacketV3> effectsPackets(uint64_t now, uint64_t session) {
        std::vector<EffectsPacketV3> packets;
        while (w.effects && start + separate_song::framesToNs(sentFrames, w.effectsRate) <= now) {
            EffectsPacketV3 e;
            e.sessionID = session;
            e.frames = 512;
            e.sampleRate = w.effectsRate;
            e.sequence = sequence++;
            e.timestamp = start + separate_song::framesToNs(sentFrames, w.effectsRate);
            for (unsigned i = 0; i < 512; ++i) {
                const double t = double(sentFrames + i) / w.effectsRate;
                e.samples[i * 2] = float(.2 * std::sin(heldout::tau * 440 * t));
                e.samples[i * 2 + 1] = w.monoEffects ? e.samples[i * 2] : float(.2 * std::sin(heldout::tau * 554 * t));
            }
            packets.push_back(e);
            sentFrames += 512;
        }
        return packets;
    }
};

// Each step of the pipeline on simulated time, on this thread, so its CPU time is the plugin's alone.
// The receiver step is handling the game's packets (checking tags, queueing; the game signs them
// beforehand, untimed); music is the decoder worker's block, effects the effects worker's, and mix the
// output thread's.
static void render(unsigned seconds, const std::string &only) {
    writeStereoWav("artifacts/audio-tests/bench-48k.wav", 48000);
    writeStereoWav("artifacts/audio-tests/bench-44k.wav", 44100);
    std::cout << "pipeline on simulated time, " << seconds
              << " s each: CPU ms per second of audio; per-block us p50/p95/p99/max; heap allocations per block\n";
    for (const auto &w : workloads()) {
        if (std::string(w.name).find(only) == std::string::npos)
            continue;
        Simulation sim;
        sim.synchronize(sim.local);
        Game game(w, sim.now);
        const uint64_t warmup = 100;
        uint64_t next = sim.now - SongSource::bufferingNs + SongSource::blockNs;
        std::map<std::string, Samples> steps;
        std::map<std::string, uint64_t> cpu, allocations;
        auto timed = [&](const char *step, bool measured, auto &&work) {
            const auto cpuBegin = cpuNs();
            const auto begin = wallNs();
            pitchAllocationCount = 0;
            pitchCountingAllocations = measured;
            work();
            pitchCountingAllocations = false;
            const auto elapsed = wallNs() - begin, used = cpuNs() - cpuBegin;
            if (!measured)
                return;
            steps[step].add(double(elapsed) / 1000);
            cpu[step] += used;
            allocations[step] += pitchAllocationCount;
        };
        const uint64_t blocks = uint64_t(seconds) * 100 + warmup;
        for (uint64_t block = 0; block < blocks; ++block) {
            const bool measured = block >= warmup;
            if (w.voices || w.effects || w.groupStage) {
                auto songs = game.songPackets(sim.now);
                for (auto &p : songs) {
                    p.sessionID = sim.local.session;
                    p.timestamp = sim.now;
                    signPacket(sim.local.key, p);
                }
                auto effects = game.effectsPackets(sim.now, sim.local.session);
                const size_t effectsSize = offsetof(EffectsPacketV3, samples) + 512 * 8;
                for (auto &e : effects)
                    signPacket(sim.local.key, e, effectsSize);
                timed("receiver", measured, [&] {
                    for (const auto &p : songs)
                        BenchAccess::handle(*receiver, &p, sizeof(p), sim.local.port, sim.now);
                    for (const auto &e : effects)
                        BenchAccess::handle(*receiver, &e, effectsSize, sim.local.effectsPort, sim.now);
                });
            }
            sim.advance(SongSource::blockNs);
            AudioBlock music{}, effects{};
            timed("music", measured, [&] { sim.source->renderMusic(next, music, sim.now); });
            timed("effects", measured, [&] { sim.source->renderEffects(next, effects); });
            const auto generation = sim.source->outputGeneration.load();
            sim.source->musicBlocks.push(next, music, generation, next);
            sim.source->effectsBlocks.push(next, effects, generation, next);
            timed("mix", measured, [&] { (void)sim.source->mixBlock(next); });
            next += SongSource::blockNs;
        }
        std::printf("%s\n", w.name);
        double total = 0;
        for (const char *step : {"receiver", "music", "effects", "mix"}) {
            if (!steps.count(step))
                continue;
            const double ms = double(cpu[step]) / 1e6 / seconds;
            total += ms;
            std::printf("  %-8s %7.2f ms/s | %s | %.2f allocs/block\n", step, ms, steps[step].spread().c_str(),
                        double(allocations[step]) / double(steps[step].values.size()));
        }
        std::printf("  total    %7.2f ms/s\n", total);
    }
}

// The real output thread and both workers, with a real receiver and a game on loopback, for misses,
// wake-ups and how late each block reaches OBS. CPU is per thread over the measured stretch; the
// game's sender threads are not the plugin's and are left out. The output thread's figure includes
// the test stub's copy of each block and the QA recount of late effects, so it compares builds rather
// than predicting OBS. Allocation counts elsewhere cover C++ `new` on the calling thread only.
static void threads(const Workload &w, unsigned seconds) {
    receiver = std::make_shared<Receiver>();
    resetOutput();
    SyncedSender sender(0);
    if (!sender.synchronize()) {
        std::cout << "  sender did not synchronize\n";
        return;
    }
    auto effectsSocket = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(receiverPort());
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Game game(w, os_gettime_ns());
    {
        SongSource source(nullptr);
        source.active = true;
        source.worker = std::thread([&] { source.run(); });
        std::atomic<bool> stop{false};
        // Back-to-back effects packets at their own cadence, as GD's mixer sends them.
        std::thread effectsSender([&] {
            const AudioThreadScheduling scheduling;
            while (!stop) {
                for (auto e : game.effectsPackets(os_gettime_ns(), sender.session)) {
                    const int size = int(offsetof(EffectsPacketV3, samples) + e.frames * 8);
                    signPacket(processLinkKey(), e, size);
                    sendto(effectsSocket, reinterpret_cast<const char *>(&e), size, 0,
                           reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        const auto start = os_gettime_ns();
        bool measuring = false;
        std::map<std::string, uint64_t> cpuBegin;
        uint64_t measureStart = 0, musicMisses = 0, effectsMisses = 0;
        auto threadCpu = [&] {
            while (!source.testWorkersStarted.load(std::memory_order_acquire))
                std::this_thread::yield();
            return std::map<std::string, uint64_t>{{"output", cpuNs(source.worker)},
                                                    {"music", cpuNs(source.decoderWorker)},
                                                    {"effects", cpuNs(source.effectsWorker)},
                                                    {"receiver", cpuNs(BenchAccess::worker(*receiver))}};
        };
        for (;;) {
            const auto now = os_gettime_ns();
            if (now - start >= uint64_t(seconds + 1) * 1000000000ULL)
                break;
            if (!measuring && now - start >= 1000000000ULL) {
                source.testMaxMixNs = 0;
                source.testWakeMaxNs = 0;
                source.testLateFrames = 0;
                musicMisses = source.testMusicMissesRaw;
                effectsMisses = source.testEffectsMissesRaw;
                source.testMeasure = true;
                cpuBegin = threadCpu();
                measureStart = now;
                resetOutput();
                measuring = true;
            }
            for (const auto &p : game.songPackets(now))
                sender.send(p, now);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto cpuEnd = threadCpu();
        const double measuredSeconds = double(os_gettime_ns() - measureStart) / 1e9;
        source.testMeasure = false;
        stop = true;
        effectsSender.join();
        Samples lateness;
        for (const auto &block : capturedBlocks())
            lateness.add(double(int64_t(block.wall) - int64_t(block.timestamp)) / 1e6);
        std::printf("%s\n", w.name);
        for (const auto &[name, begin] : cpuBegin)
            std::printf("  %-8s %7.2f ms CPU/s\n", name.c_str(), double(cpuEnd.at(name) - begin) / 1e6 / measuredSeconds);
        std::printf("  max mix %.3f ms, max wake lateness %.3f ms, music misses %llu, effects misses %llu, late effects "
                    "frames %llu\n",
                    double(source.testMaxMixNs) / 1e6, double(source.testWakeMaxNs) / 1e6,
                    static_cast<unsigned long long>(source.testMusicMissesRaw - musicMisses),
                    static_cast<unsigned long long>(source.testEffectsMissesRaw - effectsMisses),
                    static_cast<unsigned long long>(source.testLateFrames.load()));
        std::printf("  block reaches OBS after its timestamp, ms p50/p95/p99/max: %s\n", lateness.spread().c_str());
    }
    closeSocket(effectsSocket);
    receiver.reset();
}

// Tagging packets: the mod signs every song and effects packet and the plugin checks each one.
static void link(unsigned rounds) {
    SongLinkPacket song;
    std::strcpy(song.path, "C:/Users/someone/AppData/Local/GeometryDash/1234567.mp3");
    EffectsPacketV3 effects;
    effects.frames = 512;
    const size_t effectsSize = offsetof(EffectsPacketV3, samples) + 512 * 8;
    const auto &key = processLinkKey();
    auto time = [&](const char *what, auto &&work) {
        Samples calls;
        const auto begin = cpuNs();
        for (unsigned i = 0; i < rounds; ++i) {
            const auto start = wallNs();
            work();
            calls.add(double(wallNs() - start) / 1000);
        }
        std::printf("  %-34s %7.2f us CPU each | us p50/p95/p99/max %s\n", what,
                    double(cpuNs() - begin) / 1000 / rounds, calls.spread().c_str());
    };
    // Every round changes a hashed field, and every tag and check result goes into a checksum printed
    // at the end, so no compiler can hoist or drop the work. A check costs the same whether it matches.
    unsigned checksum = 0;
    std::cout << "link tags, " << rounds << " rounds\n";
    time("sign a song packet (mod)", [&] {
        signPacket(key, song);
        checksum += song.tag[0];
        ++song.attempt;
    });
    time("check a song packet (plugin)", [&] {
        checksum += signedWith(key, song);
        ++song.attempt;
    });
    time("sign a 512-frame effects packet (mod)", [&] {
        signPacket(key, effects, effectsSize);
        checksum += effects.tag[0];
        ++effects.sequence;
    });
    time("check a 512-frame effects packet", [&] {
        checksum += signedWith(key, effects, effectsSize);
        ++effects.sequence;
    });
    std::printf("  (checksum %u)\n", checksum);
}

// FMOD's recordings against the stage, by fixed alignment: no lag search, no gain matching.
static void fidelity() {
    using namespace pitch_fixtures;
    struct Case {
        const char *input, *output;
        float pitch;
        uint32_t fft;
        size_t from, frames;
    };
    const Case cases[] = {{"noise-in.wav", "noise-fft4096-p100.wav", 1.f, 4096, 24000, 48000},
                          {"noise-in.wav", "noise-fft1024-p75.wav", .75f, 1024, 24000, 48000},
                          {"noise-in.wav", "noise-fft4096-p75.wav", .75f, 4096, 24000, 48000},
                          {"sine-in.wav", "sine-fft4096-p75.wav", .75f, 4096, 24000, 48000},
                          {"burst-gap-in.wav", "burst-gap-out.wav", .75f, 4096, 0, 0}};
    std::cout << "against FMOD 2.02.31 recordings (left and right of the stage vs FMOD's mono output)\n";
    for (const auto &c : cases) {
        const auto input = read(c.input), expected = read(c.output);
        const size_t frames = c.frames ? c.frames : input.size();
        for (bool reference : {true, false}) {
            const auto out = reference ? heldout::shift<reference_41b8647::PitchShifter>(stereo(input), c.pitch, c.fft, 256, {256})
                                       : heldout::shift<PitchShifter>(stereo(input), c.pitch, c.fft, 256, {256});
            for (size_t ch = 0; ch < 2; ++ch) {
                double xy = 0, xx = 0, yy = 0, peak = 0, squared = 0;
                for (size_t f = 0; f < frames; ++f) {
                    const double x = out[(c.from + f) * 2 + ch], y = expected[f];
                    xy += x * y, xx += x * x, yy += y * y;
                    peak = std::max(peak, std::abs(x - y));
                    squared += (x - y) * (x - y);
                }
                std::printf("  %-24s %s %s: correlation %.6f, level %+.4f dB, max error %.5f, RMS error %.6f\n",
                            c.output, reference ? "41b8647 " : "shipping", ch ? "R" : "L", xy / std::sqrt(xx * yy),
                            10 * std::log10(xx / yy), peak, std::sqrt(squared / double(frames)));
            }
        }
    }
    std::cout << "against the 41b8647 reference on held-out audio (+0 and -0 count as equal)\n";
    for (const auto &[name, signal] : heldout::suite(240000)) {
        heldout::Difference difference;
        for (uint32_t fft : {256u, 512u, 1024u, 2048u, 4096u})
            for (float pitch : {.5f, .75f, 1.f, 1.25f, 1.5f, 2.f})
                for (size_t gate : {256, 480})
                    for (const auto &calls : heldout::callPatterns)
                        difference.add(heldout::shift<PitchShifter>(signal, pitch, fft, gate, calls),
                                       heldout::shift<reference_41b8647::PitchShifter>(signal, pitch, fft, gate, calls));
        std::printf("  %-18s %zu differing samples, max difference %g\n", name, difference.samples, difference.max);
    }
    heldout::Difference chains;
    for (const auto &signal : {heldout::noise(480000, 13), heldout::mono(480000), heldout::monoThenStereo(480000)})
        chains.add(heldout::chain<PitchChain>(signal), heldout::chain<reference_41b8647::PitchChain>(signal));
    std::printf("  %-18s %zu differing samples, max difference %g\n", "chain changes", chains.samples, chains.max);
}

// What a song change costs the music worker, which renders 20 ms ahead: opening the file, the seek to
// the position GD reports, and the first block, then a seek far away and one back near the start.
// Wall milliseconds per step; the first open of each file is reported apart, since the OS may not
// have cached the file yet.
static void open(unsigned rounds, const std::vector<std::string> &paths) {
    std::cout << "song open, wall ms p50/p95/p99/max over " << rounds << " rounds (first open apart)\n";
    for (const auto &path : paths) {
        Samples opens, seeks, firstBlocks, farSeeks, backSeeks;
        double firstOpen = 0;
        std::array<float, 480 * 2> block{};
        for (unsigned round = 0; round <= rounds; ++round) {
            Decoder decoder;
            auto begin = wallNs();
            const bool ok = decoder.open(path);
            const double openMs = double(wallNs() - begin) / 1e6;
            if (!ok) {
                std::printf("%s: cannot open (%s)\n", path.c_str(), decoder.error.c_str());
                break;
            }
            auto step = [&](Samples &samples, auto &&work) {
                const auto start = wallNs();
                work();
                samples.add(double(wallNs() - start) / 1e6);
            };
            if (round == 0) {
                firstOpen = openMs;
                continue;
            }
            opens.add(openMs);
            step(seeks, [&] { decoder.seek(30); });
            step(firstBlocks, [&] { decoder.render(block.data(), 480, 1); });
            step(farSeeks, [&] { decoder.seek(120); decoder.render(block.data(), 480, 1); });
            step(backSeeks, [&] { decoder.seek(5); decoder.render(block.data(), 480, 1); });
        }
        std::printf("%s\n  first open %8.2f\n  open       %s\n  seek 30 s  %s\n  first block%s\n"
                    "  seek 120 s + block %s\n  seek 5 s + block   %s\n",
                    path.c_str(), firstOpen, opens.spread().c_str(), seeks.spread().c_str(),
                    firstBlocks.spread().c_str(), farSeeks.spread().c_str(), backSeeks.spread().c_str());
    }
}
} // namespace bench

int main(int argc, char **argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
#ifdef _WIN32
    _putenv_s("OBS_JUKEBOX_TEST_PORT", "49178");
    _putenv_s("OBS_JUKEBOX_TEST_KEY_FILE", "artifacts/audio-tests/bench-link-key");
    timeBeginPeriod(1);
    bench::cyclesPerNs();
#else
    setenv("OBS_JUKEBOX_TEST_PORT", "49178", 1);
    setenv("OBS_JUKEBOX_TEST_KEY_FILE", "artifacts/audio-tests/bench-link-key", 1);
#endif
    writeLinkKey(processLinkKey());
    PitchShifter::prepareTables();
    // A count is digits only, so a GD song named 941913.mp3 stays a file name.
    const bool counted = argc > 2 && *argv[2] && std::strspn(argv[2], "0123456789") == std::strlen(argv[2]);
    const unsigned count = counted ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 0;
    if (mode == "pitch")
        bench::pitch(count ? count : 5);
    else if (mode == "render")
        bench::render(count ? count : 10, argc > 3 ? argv[3] : ""); // a third argument picks workloads by name
    else if (mode == "threads") {
        std::cout << "real threads, " << (count ? count : 8) << " s measured after 1 s\n";
        for (const auto &w : bench::workloads())
            if (w.stress || std::string(w.name) == "1 voice MP3 + effects" ||
                std::string(w.name) == "effects, 1 stage 4096 (centred SFX)")
                bench::threads(w, count ? count : 8);
    } else if (mode == "link")
        bench::link(count ? count : 20000);
    else if (mode == "fidelity")
        bench::fidelity();
    else if (mode == "open") // song files follow, after the rounds if given
        bench::open(count ? count : 20, std::vector<std::string>(argv + std::min(argc, counted ? 3 : 2), argv + argc));
    else {
        std::cerr << "Usage: bench pitch|render|threads|link|fidelity|open [rounds or seconds] [render workload name "
                     "part | song files]\n";
        return 2;
    }
    return failures ? 1 : 0;
}
