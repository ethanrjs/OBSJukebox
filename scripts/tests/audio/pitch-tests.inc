// Pitch shifter behaviour, tested against recordings of GD's own FMOD pitch shifter and against
// hand-built references. Requires separate-song.cpp and PitchShifter.hpp from playback-tests.cpp.
//
// Counts heap use on the calling thread. The stages must allocate when they are first built or
// used (a decoder voice builds its chain lazily) but never again from process() or reset().
#ifdef _WIN32
#include <malloc.h>
#endif
static thread_local bool pitchCountingAllocations = false;
static thread_local size_t pitchAllocationCount = 0, pitchFreeCount = 0;
void *operator new(size_t size) {
    if (pitchCountingAllocations) ++pitchAllocationCount;
    if (void *pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *pointer) noexcept {
    if (pitchCountingAllocations) ++pitchFreeCount;
    std::free(pointer);
}
void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
void operator delete(void *pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void *pointer, size_t) noexcept { ::operator delete(pointer); }
#ifdef _WIN32
void *operator new(size_t size, std::align_val_t alignment) {
    if (pitchCountingAllocations) ++pitchAllocationCount;
    if (void *pointer = _aligned_malloc(size ? size : 1, size_t(alignment))) return pointer;
    throw std::bad_alloc();
}
void operator delete(void *pointer, std::align_val_t) noexcept {
    if (pitchCountingAllocations) ++pitchFreeCount;
    _aligned_free(pointer);
}
#else
void *operator new(size_t size, std::align_val_t alignment) {
    if (pitchCountingAllocations) ++pitchAllocationCount;
    void *pointer = nullptr;
    if (posix_memalign(&pointer, size_t(alignment), size ? size : 1) == 0) return pointer;
    throw std::bad_alloc();
}
void operator delete(void *pointer, std::align_val_t) noexcept {
    if (pitchCountingAllocations) ++pitchFreeCount;
    std::free(pointer);
}
#endif
void *operator new[](size_t size, std::align_val_t alignment) { return ::operator new(size, alignment); }
void operator delete[](void *pointer, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }
void operator delete(void *pointer, size_t, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }
void operator delete[](void *pointer, size_t, std::align_val_t alignment) noexcept { ::operator delete(pointer, alignment); }

namespace pitch_fixtures {
const std::string dir = "scripts/tests/audio/fixtures/fmod-pitchshift/";
// A mono 16-bit WAV written by tools/qa/fmod-pitchshift-reference.cpp. An unreadable file is
// reported as a failed check and returns no samples, so the dependent comparison fails too.
static std::vector<float> readFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<float> samples;
    if (!in) { check(false, ("fixture opens: " + path).c_str()); return samples; }
    auto u32 = [&] { uint32_t v = 0; in.read(reinterpret_cast<char *>(&v), 4); return v; };
    auto u16 = [&] { uint16_t v = 0; in.read(reinterpret_cast<char *>(&v), 2); return v; };
    char riff[4]{}, wave[4]{}, chunk[4]{};
    in.read(riff, 4);
    const auto riffSize = u32();
    in.read(wave, 4);
    if (std::string(riff, 4) != "RIFF" || std::string(wave, 4) != "WAVE" || riffSize == 0) {
        check(false, ("fixture is a WAV: " + path).c_str());
        return samples;
    }
    uint32_t dataBytes = 0;
    int formatCode = 0, bitsPerSample = 0, channels = 0;
    for (;;) {
        in.read(chunk, 4);
        if (!in) break;
        const auto size = u32();
        if (std::string(chunk, 4) == "fmt ") {
            const auto format = u16(), channelCount = u16();
            const auto rate = u32();
            u32();
            u16();
            const auto bits = u16();
            if (size < 16) { check(false, ("fixture format chunk is complete: " + path).c_str()); return samples; }
            if (size > 16) in.seekg(size - 16, std::ios::cur);
            formatCode = format, bitsPerSample = bits, channels = channelCount;
            if (channels < 1 || channels > 2 || rate != 48000 ||
                !((format == 1 && bits == 16) || (format == 3 && bits == 32))) {
                check(false, ("fixture is 48 kHz mono or stereo PCM: " + path).c_str());
                return samples;
            }
        } else if (std::string(chunk, 4) == "data") {
            if (size > 100000000) { check(false, ("fixture size is sane: " + path).c_str()); return samples; }
            dataBytes = size;
            break;
        } else {
            in.seekg(size, std::ios::cur);
        }
    }
    const size_t sampleBytes = size_t(bitsPerSample / 8) * size_t(channels);
    if (!formatCode || !dataBytes || dataBytes % sampleBytes) {
        check(false, ("fixture has formatted, frame-aligned samples: " + path).c_str());
        return samples;
    }
    const size_t frames = dataBytes / sampleBytes;
    samples.resize(frames);
    if (formatCode == 3) {
        std::vector<float> pcm(frames * size_t(channels));
        in.read(reinterpret_cast<char *>(pcm.data()), std::streamsize(dataBytes));
        for (size_t i = 0; i < frames; ++i)
            samples[i] = pcm[i * size_t(channels)];
    } else {
        std::vector<int16_t> pcm(frames * size_t(channels));
        in.read(reinterpret_cast<char *>(pcm.data()), std::streamsize(dataBytes));
        for (size_t i = 0; i < frames; ++i)
            samples[i] = float(pcm[i * size_t(channels)]) / 32767.f;
    }
    if (!in) { samples.clear(); check(false, ("fixture data reads: " + path).c_str()); return samples; }
    return samples;
}
static std::vector<float> read(const char *name) { return readFile(dir + name); }
// The input FMOD processed, as stereo frames: both channels carry the same mono recording.
static std::vector<float> stereo(const std::vector<float> &mono) {
    std::vector<float> audio(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i)
        audio[i * 2] = audio[i * 2 + 1] = mono[i];
    return audio;
}
// Correlation against the reference over `frames` starting at `from`, left channel, lag zero.
static void compare(const char *label, const std::vector<float> &audio, size_t audioFrom,
                    const std::vector<float> &reference, size_t frames, double minCorrelation, double maxLevelDb) {
    if (audio.size() < (audioFrom + frames) * 2 || reference.size() < frames) {
        check(false, label);
        return;
    }
    double xy = 0, xx = 0, yy = 0;
    for (size_t f = 0; f < frames; ++f) {
        const double x = audio[(audioFrom + f) * 2], y = reference[f];
        xy += x * y, xx += x * x, yy += y * y;
    }
    const double correlation = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
    const double level = yy > 0 && xx > 0 ? 10 * std::log10(xx / yy) : 100;
    if (correlation < minCorrelation || std::abs(level) > maxLevelDb)
        std::cerr << label << ": correlation " << correlation << ", level " << level << " dB\n";
    check(correlation >= minCorrelation && std::abs(level) <= maxLevelDb, label);
}
} // namespace pitch_fixtures

// Runs a stage over the whole recording in FMOD's 256-frame blocks, as the recording tool did.
static std::vector<float> pitchReference(const std::vector<float> &mono, float pitch, uint32_t fft,
                                         size_t gateFrames = 256) {
    auto audio = pitch_fixtures::stereo(mono);
    PitchShifter shifter(gateFrames);
    for (size_t at = 0; at < mono.size(); at += 256)
        shifter.process(audio.data() + at * 2, std::min<size_t>(256, mono.size() - at), pitch, fft);
    return audio;
}

static void pitchParityTest() {
    using namespace pitch_fixtures;
    const auto noise = read("noise-in.wav");
    if (!noise.empty()) {
        const auto one = read("noise-fft4096-p100.wav");
        compare("a pitch of 1 is processed like FMOD, not passed through",
                pitchReference(noise, 1.f, 4096), 24000, one, 48000, .998, .1);
        const auto low1024 = read("noise-fft1024-p75.wav");
        compare("noise at 0.75 with FFT 1024 matches FMOD",
                pitchReference(noise, .75f, 1024), 24000, low1024, 48000, .98, .1);
        const auto low4096 = read("noise-fft4096-p75.wav");
        compare("noise at 0.75 with FFT 4096 matches FMOD",
                pitchReference(noise, .75f, 4096), 24000, low4096, 48000, .98, .1);
    }
    const auto sine = read("sine-in.wav");
    const auto sineOut = read("sine-fft4096-p75.wav");
    if (!sine.empty() && !sineOut.empty())
        compare("a 440 Hz sine at 0.75 matches FMOD",
                pitchReference(sine, .75f, 4096), 24000, sineOut, 48000, .999, .1);
    const auto gap = read("burst-gap-in.wav"), gapOut = read("burst-gap-out.wav");
    if (!gap.empty() && !gapOut.empty())
        compare("a gap holds the stage's audio until the next sound, like FMOD",
                pitchReference(gap, .75f, 4096), 0, gapOut, gap.size(), .98, .1);
}

// A silence block is gated: it outputs zeros and does not move the channel's state, and the other
// channel keeps running.
static void pitchChannelTest() {
    constexpr size_t frames = 24000;
    std::vector<float> tone(frames);
    for (size_t i = 0; i < frames; ++i)
        tone[i] = float(.3 * std::sin(2 * 3.14159265358979 * 440 * double(i) / 48000));
    auto run = [&](bool leftSilent, bool rightSilent) {
        auto audio = pitch_fixtures::stereo(tone);
        for (size_t i = 0; i < frames; ++i) {
            if (leftSilent) audio[i * 2] = 0;
            if (rightSilent) audio[i * 2 + 1] = 0;
        }
        PitchShifter shifter(256);
        for (size_t at = 0; at < frames; at += 256)
            shifter.process(audio.data() + at * 2, std::min<size_t>(256, frames - at), .75f, 4096);
        return audio;
    };
    const auto both = run(false, false), leftOff = run(true, false), rightOff = run(false, true);
    double leftError = 0, rightError = 0;
    bool silentLeft = true, silentRight = true;
    for (size_t i = 0; i < frames; ++i) {
        rightError = std::max(rightError, std::abs(double(leftOff[i * 2 + 1]) - both[i * 2 + 1]));
        leftError = std::max(leftError, std::abs(double(rightOff[i * 2]) - both[i * 2]));
        silentLeft &= leftOff[i * 2] == 0;
        silentRight &= rightOff[i * 2 + 1] == 0;
    }
    check(rightError < 1e-6 && leftError < 1e-6 && silentLeft && silentRight,
          "a silent channel freezes alone and the other channel is unaffected");
    // A channel that was silent and then gets a sound again must produce audio, not stay wedged.
    std::vector<float> resumed(frames * 2, 0.f);
    for (size_t i = 0; i < frames; ++i)
        resumed[i * 2] = i < frames / 2 ? 0.f : tone[i];
    PitchShifter shifter(256);
    for (size_t at = 0; at < frames; at += 256)
        shifter.process(resumed.data() + at * 2, std::min<size_t>(256, frames - at), .75f, 4096);
    bool audible = false;
    for (size_t i = frames * 3 / 4; i < frames; ++i)
        audible |= std::abs(resumed[i * 2]) > 1e-4f;
    check(audible, "a channel that resumes after a silent span plays again");
}

// Block sizes are OBS's choice, not FMOD's, so the stage must not depend on where a call ends.
static void pitchSplitTest() {
    constexpr size_t frames = 24000;
    std::vector<float> tone(frames);
    for (size_t i = 0; i < frames; ++i)
        tone[i] = float(.25 * std::sin(2 * 3.14159265358979 * 330 * double(i) / 48000));
    auto run = [&](const std::vector<size_t> &chunks) {
        auto audio = pitch_fixtures::stereo(tone);
        PitchShifter shifter(256);
        size_t at = 0, chunk = 0;
        while (at < frames) {
            const size_t count = std::min(chunks[chunk++ % chunks.size()], frames - at);
            shifter.process(audio.data() + at * 2, count, .8f, 2048);
            at += count;
        }
        return audio;
    };
    // Each call is long enough that a tone cannot sum below the gate: a one-frame call at a zero
    // crossing would legitimately be treated as silence, which is the documented divergence.
    const std::vector<size_t> whole{frames}, uneven{64, 113, 256, 480, 31};
    const auto one = run(whole), split = run(uneven);
    double error = 0;
    for (size_t i = 0; i < one.size(); ++i)
        error = std::max(error, std::abs(double(one[i]) - split[i]));
    check(error < 1e-6, "splitting a block into calls of any size gives the same audio");
}

// A stage that is rebuilt, moved to another FFT size, or left out of a list starts as fresh state;
// a pitch-only change keeps the audio and phases it is holding.
static void pitchResetTest() {
    constexpr size_t frames = 12000;
    auto noise = [&](unsigned seed) {
        std::vector<float> mono(frames);
        std::mt19937 generator(seed);
        std::uniform_real_distribution<float> value(-.2f, .2f);
        for (auto &sample : mono)
            sample = value(generator);
        return mono;
    };
    const auto first = noise(1), second = noise(2);
    auto run = [&](PitchShifter &shifter, const std::vector<float> &mono, float pitch, uint32_t fft) {
        auto audio = pitch_fixtures::stereo(mono);
        for (size_t at = 0; at < frames; at += 256)
            shifter.process(audio.data() + at * 2, std::min<size_t>(256, frames - at), pitch, fft);
        return audio;
    };
    PitchShifter fresh(256);
    const auto expected = run(fresh, second, .75f, 1024);
    PitchShifter reused(256);
    run(reused, first, .75f, 1024);
    reused.reset(1024);
    check(run(reused, second, .75f, 1024) == expected, "a reset stage matches a fresh stage");
    run(reused, first, 1.5f, 4096);
    reused.reset(1024);
    check(run(reused, second, .75f, 1024) == expected, "an FFT change starts from fresh state");

    // Same instance and FFT with a new pitch must keep its history: a reset would match the fresh run.
    PitchStage stage{.75f, 1024, 0, 41};
    auto throughChain = [&](const std::vector<float> &firstHalf, const std::vector<float> &secondHalf, float next) {
        PitchChain chain(true, 256);
        auto audio = pitch_fixtures::stereo(firstHalf);
        for (size_t at = 0; at < frames; at += 256)
            chain.process(audio.data() + at * 2, std::min<size_t>(256, frames - at), &stage, 1);
        auto later = pitch_fixtures::stereo(secondHalf);
        stage.pitch = next;
        for (size_t at = 0; at < frames; at += 256)
            chain.process(later.data() + at * 2, std::min<size_t>(256, frames - at), &stage, 1);
        return later;
    };
    stage.pitch = .75f;
    const auto continued = throughChain(first, second, .9f);
    // The same calls through one stage must give exactly the same audio: a reset or a rebuilt
    // stage would differ.
    PitchShifter manual(256);
    auto reference = pitch_fixtures::stereo(first);
    for (size_t at = 0; at < frames; at += 256)
        manual.process(reference.data() + at * 2, std::min<size_t>(256, frames - at), .75f, 1024);
    reference = pitch_fixtures::stereo(second);
    for (size_t at = 0; at < frames; at += 256)
        manual.process(reference.data() + at * 2, std::min<size_t>(256, frames - at), .9f, 1024);
    check(continued == reference, "a pitch-only change keeps the stage's history");

    // Leaving a stage out of the list and putting it back resets it, even with the same instance.
    PitchStage pair[2] = {{.75f, 1024, 0, 41}, {1.1f, 2048, 0, 42}};
    PitchChain absent(true, 256);
    auto warm = pitch_fixtures::stereo(first);
    for (size_t at = 0; at < frames; at += 256)
        absent.process(warm.data() + at * 2, std::min<size_t>(256, frames - at), pair, 2);
    auto gone = pitch_fixtures::stereo(second);
    for (size_t at = 0; at < frames; at += 256)
        absent.process(gone.data() + at * 2, std::min<size_t>(256, frames - at), nullptr, 0);
    auto back = pitch_fixtures::stereo(second);
    for (size_t at = 0; at < frames; at += 256)
        absent.process(back.data() + at * 2, std::min<size_t>(256, frames - at), pair, 2);
    PitchChain rebuilt(true, 256);
    auto freshBack = pitch_fixtures::stereo(second);
    for (size_t at = 0; at < frames; at += 256)
        rebuilt.process(freshBack.data() + at * 2, std::min<size_t>(256, frames - at), pair, 2);
    check(back == freshBack, "a stage that returns after an absence starts fresh");
}

// Two stages are applied in order, not multiplied into one.
static void pitchOrderTest() {
    constexpr size_t frames = 24000;
    std::vector<float> tone(frames);
    for (size_t i = 0; i < frames; ++i)
        tone[i] = float(.2 * std::sin(2 * 3.14159265358979 * 220 * double(i) / 48000) +
                        .1 * std::sin(2 * 3.14159265358979 * 660 * double(i) / 48000));
    auto throughChain = [&](bool reverse) {
        auto audio = pitch_fixtures::stereo(tone);
        PitchChain chain(true, 256);
        PitchStage stages[2] = {{.75f, 1024, 0, 1}, {1.5f, 2048, 0, 2}};
        if (reverse)
            std::swap(stages[0], stages[1]);
        for (size_t at = 0; at < frames; at += 480)
            chain.process(audio.data() + at * 2, std::min<size_t>(480, frames - at), stages, 2);
        return audio;
    };
    const auto forward = throughChain(false), reversed = throughChain(true);
    auto reference = pitch_fixtures::stereo(tone);
    PitchShifter a(256), b(256);
    for (size_t at = 0; at < frames; at += 480) {
        const size_t count = std::min<size_t>(480, frames - at);
        a.process(reference.data() + at * 2, count, .75f, 1024);
        b.process(reference.data() + at * 2, count, 1.5f, 2048);
    }
    double error = 0, difference = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
        error = std::max(error, std::abs(double(reference[i]) - forward[i]));
        difference = std::max(difference, std::abs(double(forward[i]) - reversed[i]));
    }
    check(error < 1e-6 && difference > .01, "stages run in order instead of collapsing into one pitch");
}

// A new process session must not inherit FIFO state, even for the same stage slot and FFT.
static void pitchSessionResetTest() {
    constexpr size_t frames = 12000;
    std::vector<float> first(frames), second(frames);
    for (size_t i = 0; i < frames; ++i) {
        first[i] = float(.25 * std::sin(2 * 3.14159265358979 * 330 * double(i) / 48000));
        second[i] = float(.25 * std::sin(2 * 3.14159265358979 * 550 * double(i) / 48000));
    }
    PitchStage stage{.5f, 1024, 0, 61};
    auto feed = [&](PitchChain &chain, uint64_t &session, uint64_t id, const std::vector<float> &mono,
                    float pitch) {
        stage.pitch = pitch;
        auto audio = pitch_fixtures::stereo(mono);
        for (size_t at = 0; at < frames; at += 256)
            processStages(chain, session, id, audio.data() + at * 2, std::min<size_t>(256, frames - at), &stage, 1);
        return audio;
    };
    PitchChain reused(true, 256), fresh(true, 256);
    uint64_t session = 0, freshSession = 0;
    feed(reused, session, 1, first, .5f);
    const auto actual = feed(reused, session, 2, second, 1.5f);
    const auto expected = feed(fresh, freshSession, 2, second, 1.5f);
    check(actual == expected, "a new session starts its stages from fresh state");
    // The same chain without the session change keeps its history, so the audio differs.
    PitchChain carried(true, 256);
    uint64_t oneSession = 1;
    feed(carried, oneSession, 1, first, .5f);
    const auto continued = feed(carried, oneSession, 1, second, 1.5f);
    double difference = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        difference = std::max(difference, std::abs(double(continued[i]) - expected[i]));
    check(difference > .01, "carrying a stage across a session boundary would change the audio");
}

static void pitchAllocationTest() {
    PitchShifter::prepareTables();
    PitchChain chain(true, 256);
    std::vector<float> audio(480 * 2, .2f);
    PitchStage stages[2] = {{1.f, 1024, 0, 41}, {.75f, 2048, 0, 42}};
    chain.process(audio.data(), 480, stages, 2);
    pitchAllocationCount = pitchFreeCount = 0;
    pitchCountingAllocations = true;
    chain.process(audio.data(), 480, stages, 2);
    stages[0].pitch = .9f;
    chain.process(audio.data(), 480, stages, 2);
    stages[1].instance = 43;
    chain.process(audio.data(), 480, stages, 2);
    chain.process(audio.data(), 480, stages, 1);
    chain.process(audio.data(), 480, stages, 2);
    chain.reset();
    chain.process(audio.data(), 480, stages, 2);
    stages[0].fft = 4096;
    chain.process(audio.data(), 480, stages, 2);
    pitchCountingAllocations = false;
    check(pitchAllocationCount == 0 && pitchFreeCount == 0,
          "pitch stages never allocate or free during processing or reset");
}

// Invalid calls cannot be reached through the wire, but must not read or write outside the buffer or
// leave a stage holding audio.
static void pitchInvalidInputTest() {
    PitchShifter shifter(256);
    std::vector<float> audio(512, .1f);
    shifter.process(audio.data(), 256, .75f, 1024);
    check(shifter.active(), "a valid stage is active before invalid input");
    shifter.process(nullptr, 256, .75f, 1024);
    shifter.process(audio.data(), 0, .75f, 1024);
    shifter.process(audio.data(), 256, std::numeric_limits<float>::quiet_NaN(), 1024);
    check(!shifter.active(), "a nonfinite pitch clears the stage instead of holding audio");
    shifter.process(audio.data(), 256, .75f, 3000);
    check(shifter.active(), "an FFT size between supported powers still configures a stage");
    const auto untouched = audio;
    PitchChain chain(true, 256);
    chain.process(audio.data(), 256, nullptr, 2);
    chain.process(nullptr, 256, nullptr, 0);
    check(audio == untouched, "invalid stage lists are ignored without touching audio");
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
static unsigned toneCrossings(const Simulation &sim, uint64_t from, uint64_t to) {
    std::vector<float> mono;
    for (const auto &block : sim.output)
        if (block.timestamp >= from && block.timestamp < to)
            for (size_t i = 0; i < blockFrames; ++i) mono.push_back(block.samples[i * 2]);
    unsigned count = 0;
    for (size_t i = 1; i < mono.size(); ++i)
        if (mono[i - 1] < 0 && mono[i] >= 0) ++count;
    return count;
}
static double toneHz(const Simulation &sim, uint64_t from, uint64_t to) {
    return double(toneCrossings(sim, from, to)) / (double(to - from) / 1e9);
}
// The game sends a song packet about every 10 ms with the position it describes, so the song stays
// live (packets go stale after 400 ms) and the decoder is never asked to seek.
static void heartbeat(Simulation &sim, SongLinkPacket p, uint64_t base) {
    p.position = double(sim.now - base) / 1e9;
    sim.send(p, sim.now);
}
static void playTone(Simulation &sim, SongLinkPacket p, uint64_t base, uint64_t until) {
    while (sim.now < base + until) {
        heartbeat(sim, p, base);
        sim.advance(10000000);
    }
}
static void pitchRenderChannelTest() {
    Simulation sim;
    const auto base = sim.now;
    SongLinkPacket p;
    p.flags = 3; p.epoch = 1; p.stageCount = 1;
    p.stages[0] = {.5f, 1024, 0, 7};
    std::strcpy(p.path, "artifacts/audio-tests/tone.wav");
    sim.start();
    playTone(sim, p, base, 1200000000);
    const double hz = toneHz(sim, base + 300000000, base + 1100000000);
    if (std::abs(hz - 220) > 18) std::cerr << "channel stage: " << hz << " Hz\n";
    check(std::abs(hz - 220) <= 18, "a voice packet's stage shifts the music OBS plays");
}
// A global stage processes the eligible music; a voice the setting excludes stays unshifted.
static void pitchRenderGroupTest() {
    for (bool excluded : {false, true}) {
        Simulation sim;
        const auto base = sim.now;
        SongLinkPacket global;
        global.channelID = -1; global.flags = 1; global.epoch = 1;
        global.stageCount = 1; global.stages[0] = {.5f, 1024, 0, 8};
        SongLinkPacket voice;
        voice.flags = 3 | (excluded ? 8u : 0u); voice.epoch = 1; voice.stageCount = 0;
        std::strcpy(voice.path, "artifacts/audio-tests/tone.wav");
        sim.start();
        while (sim.now < base + 1200000000) {
            heartbeat(sim, global, base);
            heartbeat(sim, voice, base);
            sim.advance(10000000);
        }
        const double hz = toneHz(sim, base + 300000000, base + 1100000000);
        const double expected = excluded ? 440 : 220;
        if (std::abs(hz - expected) > 18) std::cerr << "group stage excluded=" << excluded << ": " << hz << " Hz\n";
        check(std::abs(hz - expected) <= 18,
              excluded ? "an excluded replacement bypasses the music group's stages"
                       : "a global stage shifts the eligible music once");
    }
}
static void pitchRenderEffectsTest() {
    for (bool staged : {false, true}) {
        Simulation sim;
        const auto base = sim.now;
        SongLinkPacket global;
        global.channelID = -1; global.flags = 1; global.epoch = 1;
        if (staged) {
            global.effectsStageCount = 1;
            global.effectsStages[0] = {.5f, 1024, 0, 9};
        }
        sim.start();
        for (int i = 0; i < 120; ++i) {
            heartbeat(sim, global, base);
            EffectsPacketV2 effect;
            effect.frames = 480; effect.sampleRate = 48000; effect.sequence = i;
            effect.timestamp = base + uint64_t(i) * 10000000;
            for (unsigned f = 0; f < 480; ++f)
                effect.samples[f * 2] = effect.samples[f * 2 + 1] =
                    float(.2 * std::sin(2 * 3.14159265358979 * 440 * double(i * 480 + f) / 48000));
            sim.sendEffects(effect);
            sim.advance(10000000);
        }
        sim.advance(100000000);
        const double hz = toneHz(sim, base + 300000000, base + 1100000000);
        const double expected = staged ? 220 : 440;
        if (std::abs(hz - expected) > 18) std::cerr << "effects stage=" << staged << ": " << hz << " Hz\n";
        check(std::abs(hz - expected) <= 18,
              staged ? "a global packet's effects stage shifts the SFX OBS plays"
                     : "SFX without effects stages stay unshifted");
    }
}
// A stage change inside one block must take effect at exactly its packet's frame. A dry run gives
// the input, and one PitchChain with the same schedule gives the expected output.
static void pitchRenderRunTest() {
    auto run = [&](bool staged, std::vector<std::vector<float>> &blocks,
                   std::vector<uint64_t> &times) -> uint64_t {
        Simulation sim;
        const auto base = sim.now;
        SongLinkPacket p;
        p.flags = 3; p.epoch = 1;
        std::strcpy(p.path, "artifacts/audio-tests/tone.wav");
        if (staged) {
            p.stageCount = 1;
            p.stages[0] = {.5f, 1024, 0, 11};
        }
        sim.start();
        while (sim.now < base + 1200000000) {
            if (sim.now == base + 500000000) {
                // A packet five milliseconds into the block, as the game's 10 ms packets can
                // land between OBS's blocks.
                heartbeat(sim, p, base);
                sim.advance(5000000);
                if (staged)
                    p.stages[0].pitch = 2.f;
                heartbeat(sim, p, base);
                sim.advance(5000000);
                continue;
            }
            heartbeat(sim, p, base);
            sim.advance(10000000);
        }
        for (const auto &block : sim.output) {
            blocks.push_back(block.samples);
            times.push_back(block.timestamp);
        }
        return base;
    };
    std::vector<std::vector<float>> staged, dry;
    std::vector<uint64_t> stagedTimes, dryTimes;
    const auto base = run(true, staged, stagedTimes);
    run(false, dry, dryTimes);
    PitchChain chain(true, 512);
    PitchStage first{.5f, 1024, 0, 11}, second{2.f, 1024, 0, 11};
    bool match = staged.size() == dry.size() && staged.size() > 20 && stagedTimes == dryTimes;
    for (size_t i = 0; match && i < dry.size(); ++i) {
        auto expected = dry[i];
        // The block the stage changes in is split at 505 ms; all others are wholly one stage.
        const size_t split = frameAt(dryTimes[i], base + 505000000);
        if (split)
            chain.process(expected.data(), split, &first, 1);
        if (split < blockFrames)
            chain.process(expected.data() + split * 2, blockFrames - split, &second, 1);
        for (size_t f = 0; f < expected.size(); ++f)
            match &= std::abs(double(staged[i][f]) - expected[f]) < 1e-9;
    }
    check(match, "a stage change inside a block applies from its packet's frame");
}
// Without a global packet the music has no group stages to follow, so it passes through as the
// game's channel stages leave it.
static void pitchRenderNoGlobalTest() {
    Simulation sim;
    const auto base = sim.now;
    SongLinkPacket p;
    p.flags = 3; p.epoch = 1;
    std::strcpy(p.path, "artifacts/audio-tests/tone.wav");
    sim.start();
    playTone(sim, p, base, 900000000);
    const double hz = toneHz(sim, base + 300000000, base + 800000000);
    if (std::abs(hz - 440) > 18) std::cerr << "no global: " << hz << " Hz\n";
    check(std::abs(hz - 440) <= 18, "without a global packet the music passes through unchanged");
}
// Two eligible voices and one excluded voice sharing a block. The group stage must run once on the
// eligible sum, and the excluded voice must be added after it. Each part is compared exactly
// against the same voices played alone, with the dry sum fed through one offline chain.
static void pitchRenderGroupBusTest() {
    auto run = [&](bool firstOn, bool secondOn, bool excludedOn, bool staged,
                   std::vector<std::vector<float>> &blocks, std::vector<uint64_t> &times) {
        Simulation sim;
        const auto base = sim.now;
        SongLinkPacket global;
        global.channelID = -1; global.flags = 1; global.epoch = 1;
        if (staged) {
            global.stageCount = 1;
            global.stages[0] = {.5f, 1024, 0, 12};
        }
        SongLinkPacket first;
        first.channelID = 0; first.flags = 3; first.epoch = 1;
        std::strcpy(first.path, "artifacts/audio-tests/tone.wav");
        SongLinkPacket second = first;
        second.channelID = 1;
        std::strcpy(second.path, "artifacts/audio-tests/tone-high.wav");
        SongLinkPacket excluded = first;
        excluded.channelID = 2; excluded.flags = 3 | 8;
        std::strcpy(excluded.path, "artifacts/audio-tests/tone-low.wav");
        sim.start();
        while (sim.now < base + 900000000) {
            heartbeat(sim, global, base);
            if (firstOn) heartbeat(sim, first, base);
            if (secondOn) heartbeat(sim, second, base);
            if (excludedOn) heartbeat(sim, excluded, base);
            sim.advance(10000000);
        }
        for (const auto &block : sim.output) {
            blocks.push_back(block.samples);
            times.push_back(block.timestamp);
        }
    };
    std::vector<std::vector<float>> first, second, both, excluded, all;
    std::vector<uint64_t> firstTimes, secondTimes, bothTimes, excludedTimes, allTimes;
    run(true, false, false, false, first, firstTimes);
    run(false, true, false, false, second, secondTimes);
    run(true, true, false, true, both, bothTimes);
    run(false, false, true, true, excluded, excludedTimes);
    run(true, true, true, true, all, allTimes);
    auto audible = [](const std::vector<std::vector<float>> &blocks) {
        bool heard = false;
        for (const auto &block : blocks)
            for (float sample : block)
                heard |= std::abs(sample) > .01f;
        return heard;
    };
    check(audible(first), "the group bus test plays audible music on the first eligible voice");
    check(audible(second), "the group bus test plays audible music on the second eligible voice");
    check(audible(excluded), "the group bus test plays audible music on the excluded voice");
    // One chain, fed the dry sum block by block, is what the group stage must do to the bus.
    PitchChain chain;
    PitchStage stage{.5f, 1024, 0, 12};
    bool sumMatch = both.size() == first.size() && both.size() == second.size() &&
                    bothTimes == firstTimes && bothTimes == secondTimes;
    for (size_t i = 0; sumMatch && i < both.size(); ++i) {
        auto sum = first[i];
        for (size_t f = 0; f < sum.size(); ++f)
            sum[f] += second[i][f];
        chain.process(sum.data(), blockFrames, &stage, 1);
        for (size_t f = 0; f < sum.size(); ++f)
            sumMatch &= both[i][f] == sum[f];
    }
    check(sumMatch, "the group stage runs once on the sum of the eligible voices");
    bool excludedMatch = all.size() == both.size() && all.size() == excluded.size() &&
                         allTimes == bothTimes && allTimes == excludedTimes;
    for (size_t i = 0; excludedMatch && i < all.size(); ++i)
        for (size_t f = 0; f < all[i].size(); ++f)
            excludedMatch &= all[i][f] == both[i][f] + excluded[i][f];
    check(excludedMatch, "an excluded voice is added after the group stage");
}
// The first `frames` samples above the noise floor at or after `at`, as a frequency. A resume
// plays the stage's held audio first, so this tells it from the new audio that follows.
static double firstAudibleHz(const Simulation &sim, uint64_t at, size_t frames) {
    std::vector<float> mono;
    for (const auto &block : sim.output) {
        if (block.timestamp < at)
            continue;
        for (size_t i = 0; i < blockFrames && mono.size() < frames; ++i) {
            const float sample = block.samples[i * 2];
            if (mono.empty() && std::abs(sample) <= .01f)
                continue;
            mono.push_back(sample);
        }
        if (mono.size() == frames)
            break;
    }
    if (mono.size() < frames)
        return 0;
    unsigned crossings = 0;
    for (size_t i = 1; i < mono.size(); ++i)
        crossings += mono[i - 1] < 0 && mono[i] >= 0;
    return double(crossings) / (double(frames) / 48000.);
}
// Pause freezes the stage, including the audio it is holding. The resumed file is a different
// tone, so the first audio after the resume can only be the held one.
static void pitchPauseHeldTest() {
    Simulation sim;
    const auto base = sim.now;
    const auto pauseAt = base + 500000000, resumeAt = base + 550000000;
    SongLinkPacket p;
    p.flags = 3; p.epoch = 1; p.stageCount = 1; p.stages[0] = {.5f, 4096, 0, 93};
    std::strcpy(p.path, "artifacts/audio-tests/tone.wav");
    sim.start();
    while (sim.now < pauseAt) {
        heartbeat(sim, p, base);
        sim.advance(10000000);
    }
    p.flags = 1; p.position = .5;
    sim.send(p, sim.now);
    sim.advance(resumeAt - sim.now);
    p.flags = 3; p.position = 0;
    std::strcpy(p.path, "artifacts/audio-tests/tone-high.wav");
    sim.send(p, sim.now);
    while (sim.now < base + 900000000) {
        heartbeat(sim, p, base);
        sim.advance(10000000);
    }
    const double hz = firstAudibleHz(sim, resumeAt, 1920);
    if (std::abs(hz - 220) > 25) std::cerr << "pause held: " << hz << " Hz\n";
    check(std::abs(hz - 220) <= 25, "a resume plays the stage's held audio first");
}
// A voice packet is not the global state: with no global packet, effects are not mixed at all.
static void pitchRenderNoEffectsWithoutGlobalTest() {
    Simulation sim;
    const auto base = sim.now;
    SongLinkPacket voice;
    voice.flags = 3; voice.epoch = 1; // no path: this voice plays nothing
    sim.send(voice, base);
    sim.start();
    for (int i = 0; i < 30; ++i) {
        EffectsPacketV2 effect;
        effect.frames = 480; effect.sampleRate = 48000; effect.sequence = i;
        effect.timestamp = base + uint64_t(i) * 10000000;
        for (unsigned f = 0; f < 480; ++f)
            effect.samples[f * 2] = effect.samples[f * 2 + 1] = .25f;
        sim.sendEffects(effect);
        sim.advance(10000000);
    }
    bool silent = true;
    for (const auto &block : sim.output)
        for (float sample : block.samples) silent &= sample == 0;
    check(silent, "effects are not mixed without a global packet");
}
// The first global packet can arrive inside a block. Frames before it stay dry, and the new run
// starts from a fresh chain at exactly its frame.
static void pitchFirstGlobalRunTest() {
    constexpr uint64_t offset = 173 * 1000000000ULL / 48000;
    auto run = [&](bool staged, std::vector<std::vector<float>> &blocks,
                   std::vector<uint64_t> &times) -> uint64_t {
        Simulation sim;
        const auto base = sim.now;
        SongLinkPacket voice;
        voice.flags = 3; voice.epoch = 1;
        std::strcpy(voice.path, "artifacts/audio-tests/tone.wav");
        SongLinkPacket global;
        global.channelID = -1; global.flags = 1; global.epoch = 1;
        global.stageCount = 1; global.stages[0] = {.5f, 1024, 0, 81};
        const auto change = base + 500000000 + offset;
        sim.start();
        while (sim.now < base + 1200000000) {
            if (sim.now == base + 500000000) {
                heartbeat(sim, voice, base);
                if (staged) {
                    sim.advance(offset);
                    sim.send(global, change);
                    sim.advance(10000000 - offset);
                } else {
                    sim.advance(10000000);
                }
                continue;
            }
            heartbeat(sim, voice, base);
            if (staged && sim.now > change)
                heartbeat(sim, global, base);
            sim.advance(10000000);
        }
        for (const auto &block : sim.output) {
            blocks.push_back(block.samples);
            times.push_back(block.timestamp);
        }
        return base;
    };
    std::vector<std::vector<float>> dry, staged;
    std::vector<uint64_t> dryTimes, stagedTimes;
    const auto base = run(false, dry, dryTimes);
    run(true, staged, stagedTimes);
    const auto change = base + 500000000 + offset;
    PitchChain chain;
    PitchStage stage{.5f, 1024, 0, 81};
    bool match = staged.size() == dry.size() && staged.size() > 20 && stagedTimes == dryTimes;
    for (size_t i = 0; match && i < dry.size(); ++i) {
        const size_t split = frameAt(dryTimes[i], change);
        auto expected = dry[i];
        for (size_t f = 0; f < split * 2; ++f)
            match &= staged[i][f] == expected[f];
        if (split < blockFrames) {
            chain.process(expected.data() + split * 2, blockFrames - split, &stage, 1);
            for (size_t f = split * 2; f < expected.size(); ++f)
                match &= staged[i][f] == expected[f];
        }
    }
    check(match, "the first global run starts from a fresh chain at its packet's frame");
}
// A second process session starts with no stage history. Session 1 primes the group chain, then
// goes silent long enough for the receiver to accept session 2; session 2's first global packet
// arrives mid-block and must start exactly like a fresh chain.
static void pitchSessionRenderTest() {
    constexpr uint64_t offset = 173 * 1000000000ULL / 48000;
    auto run = [&](bool primed, std::vector<std::vector<float>> &blocks,
                   std::vector<uint64_t> &times) -> uint64_t {
        Simulation sim;
        const auto base = sim.now;
        const auto secondStart = base + 2350000000ULL;
        const auto change = secondStart + 500000000ULL + offset;
        auto firstSession = sim.game();
        auto secondSession = sim.game();
        SongLinkPacket firstVoice;
        firstVoice.flags = 3; firstVoice.epoch = 1;
        std::strcpy(firstVoice.path, "artifacts/audio-tests/tone.wav");
        SongLinkPacket firstGlobal;
        firstGlobal.channelID = -1; firstGlobal.flags = 1; firstGlobal.epoch = 1;
        firstGlobal.stageCount = 1; firstGlobal.stages[0] = {.5f, 1024, 0, 91};
        SongLinkPacket secondVoice = firstVoice;
        SongLinkPacket secondGlobal = firstGlobal;
        secondGlobal.stages[0].pitch = 2.f;
        // Both runs play session 1 first, so session 2 sees the same clock and decoder history.
        // The dry run's session 1 has no stages, which is the only difference before the change.
        firstGlobal.stageCount = primed ? 1 : 0;
        sim.start();
        sim.answering = &firstSession;
        while (sim.now < base + 300000000) {
            sim.send(firstSession, firstVoice, sim.now);
            sim.send(firstSession, firstGlobal, sim.now);
            sim.advance(10000000);
        }
        if (sim.now < secondStart)
            sim.advance(secondStart - sim.now);
        sim.answering = &secondSession;
        while (sim.now < secondStart + 900000000) {
            const auto at = sim.now - secondStart;
            if (at == 500000000) {
                secondVoice.position = .5;
                sim.send(secondSession, secondVoice, sim.now);
                sim.advance(offset);
                sim.send(secondSession, secondGlobal, sim.now);
                sim.advance(10000000 - offset);
                continue;
            }
            secondVoice.position = double(at) / 1e9;
            sim.send(secondSession, secondVoice, sim.now);
            if (sim.now > change)
                sim.send(secondSession, secondGlobal, sim.now);
            sim.advance(10000000);
        }
        for (const auto &block : sim.output) {
            blocks.push_back(block.samples);
            times.push_back(block.timestamp);
        }
        return base;
    };
    std::vector<std::vector<float>> dry, primed;
    std::vector<uint64_t> dryTimes, primedTimes;
    const auto base = run(false, dry, dryTimes);
    run(true, primed, primedTimes);
    const auto secondStart = base + 2350000000ULL;
    const auto change = secondStart + 500000000ULL + offset;
    bool match = primed.size() == dry.size() && primed.size() > 20 && primedTimes == dryTimes;
    size_t block = dry.size();
    for (size_t i = 0; i < dry.size(); ++i)
        if (dryTimes[i] <= change && change < dryTimes[i] + SongSource::blockNs) {
            block = i;
            break;
        }
    match &= block < dry.size();
    if (match) {
        const size_t split = frameAt(dryTimes[block], change);
        // A fresh chain's first call outputs nothing for its latency. A chain that carried session
        // 1's held audio would output it here instead.
        double freshPeak = 0;
        for (size_t f = split * 2; f < std::min<size_t>(split * 2 + 512, blockFrames * 2); ++f)
            freshPeak = std::max(freshPeak, double(std::abs(primed[block][f])));
        PitchChain carried;
        PitchStage firstStage{.5f, 1024, 0, 91}, stage{2.f, 1024, 0, 91};
        for (size_t i = 0; i < block; ++i)
            if (dryTimes[i] >= base && dryTimes[i] < base + 300000000)
                carried.process(dry[i].data(), blockFrames, &firstStage, 1);
        auto carriedBlock = dry[block];
        carried.process(carriedBlock.data() + split * 2, blockFrames - split, &stage, 1);
        double carriedPeak = 0;
        for (size_t f = split * 2; f < std::min<size_t>(split * 2 + 512, blockFrames * 2); ++f)
            carriedPeak = std::max(carriedPeak, double(std::abs(carriedBlock[f])));
        if (freshPeak >= 1e-3 || carriedPeak <= .01)
            std::cerr << "session fresh peak " << freshPeak << ", carried peak " << carriedPeak << '\n';
        check(freshPeak < 1e-3, "a new session's first global run starts from a fresh chain");
        check(carriedPeak > .01, "carrying a stage into the new session would be audible");
    } else {
        check(false, "session test finds its first global run");
    }
}
// During a main-thread stall, song packets stop but FMOD keeps mixing and the effects tap keeps
// sending. The last global stage must keep processing the music past the 400 ms voice staleness.
static void pitchStallStagesTest() {
    Simulation sim;
    const auto base = sim.now;
    SongLinkPacket global;
    global.channelID = -1; global.flags = 1; global.epoch = 1;
    global.stageCount = 1; global.stages[0] = {.5f, 1024, 0, 82};
    SongLinkPacket voice;
    voice.flags = 3; voice.epoch = 1;
    std::strcpy(voice.path, "artifacts/audio-tests/tone.wav");
    sim.start();
    for (int i = 0; i < 90; ++i) {
        if (sim.now < base + 300000000) {
            heartbeat(sim, global, base);
            heartbeat(sim, voice, base);
        }
        EffectsPacketV2 effect;
        effect.frames = 480; effect.sampleRate = 48000; effect.sequence = i;
        effect.timestamp = sim.now;
        sim.sendEffects(effect);
        sim.advance(10000000);
    }
    const double hz = toneHz(sim, base + 700000000, base + 850000000);
    if (std::abs(hz - 220) > 18) std::cerr << "stall stage: " << hz << " Hz\n";
    check(std::abs(hz - 220) <= 18, "a main-thread stall keeps the group stage");
}
// The effects stage comes from the same global packet. Once that packet stops updating, the last
// stage must keep shifting the SFX that FMOD's mixer thread still sends.
static void pitchStallEffectsTest() {
    Simulation sim;
    const auto base = sim.now;
    SongLinkPacket voice;
    voice.flags = 1; voice.epoch = 1; // no path: this voice plays nothing
    SongLinkPacket global;
    global.channelID = -1; global.flags = 1; global.epoch = 1;
    global.effectsStageCount = 1; global.effectsStages[0] = {.5f, 1024, 0, 83};
    sim.send(voice, base);
    sim.start();
    for (int i = 0; i < 90; ++i) {
        if (sim.now < base + 300000000) heartbeat(sim, global, base);
        EffectsPacketV2 effect;
        effect.frames = 480; effect.sampleRate = 48000; effect.sequence = i;
        effect.timestamp = sim.now;
        for (unsigned f = 0; f < 480; ++f)
            effect.samples[f * 2] = effect.samples[f * 2 + 1] =
                float(.2 * std::sin(2 * 3.14159265358979 * 440 * double(i * 480 + f) / 48000));
        sim.sendEffects(effect);
        sim.advance(10000000);
    }
    const double hz = toneHz(sim, base + 700000000, base + 850000000);
    if (std::abs(hz - 220) > 18) std::cerr << "stall effects: " << hz << " Hz\n";
    check(std::abs(hz - 220) <= 18, "a main-thread stall keeps the effects stage");
}
// A retired voice is destroyed and rebuilt, so its chain starts fresh even for the same stage.
static void pitchVoiceReuseTest() {
    Simulation sim;
    const auto base = sim.now;
    SongLinkPacket p;
    p.flags = 3; p.epoch = 1; p.channelID = 0; p.stageCount = 1; p.stages[0] = {.5f, 4096, 0, 95};
    std::strcpy(p.path, "artifacts/audio-tests/tone.wav");
    sim.start();
    while (sim.now < base + 300000000) {
        heartbeat(sim, p, base);
        sim.advance(10000000);
    }
    // Restart just after the 400 ms staleness point, before a surviving stage could drain its
    // 4096-frame history. Deleting the voice reset must leave the old 220 Hz audible here.
    sim.advance(410000000);
    p.epoch = 2; p.position = 0;
    std::strcpy(p.path, "artifacts/audio-tests/tone-high.wav");
    const auto restartAt = sim.now;
    sim.send(p, sim.now);
    while (sim.now < base + 1300000000) {
        heartbeat(sim, p, base);
        sim.advance(10000000);
    }
    const double hz = firstAudibleHz(sim, restartAt, 2880);
    if (std::abs(hz - 330) > 25) std::cerr << "voice reuse: " << hz << " Hz\n";
    check(std::abs(hz - 330) <= 25, "a reused voice starts its stage fresh");
}
static void pitchRenderTests() {
    writeTone("artifacts/audio-tests/tone.wav");
    writeTone("artifacts/audio-tests/tone-high.wav", 660);
    writeTone("artifacts/audio-tests/tone-low.wav", 330);
    pitchRenderChannelTest();
    pitchRenderGroupTest();
    pitchRenderEffectsTest();
    pitchRenderRunTest();
    pitchRenderNoGlobalTest();
    pitchRenderGroupBusTest();
    pitchRenderNoEffectsWithoutGlobalTest();
    pitchFirstGlobalRunTest();
    pitchSessionRenderTest();
    pitchStallStagesTest();
    pitchStallEffectsTest();
    pitchPauseHeldTest();
    pitchVoiceReuseTest();
}

static void pitchTests() {
    PitchShifter::prepareTables();
    pitchParityTest();
    pitchChannelTest();
    pitchSplitTest();
    pitchResetTest();
    pitchSessionResetTest();
    pitchOrderTest();
    pitchAllocationTest();
    pitchInvalidInputTest();
    pitchRenderTests();
}
