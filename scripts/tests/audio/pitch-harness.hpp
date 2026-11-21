#pragma once
#include "harness.hpp"
#ifdef _WIN32
#include <malloc.h>
#endif
#include "reference/PitchShifter-41b8647.hpp"
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
// The input FMOD processed: both channels carry the same mono recording.
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
}

// Held-out signals the FMOD recordings don't cover, for checking the shipping stage against the
// frozen 41b8647 reference. Interleaved stereo at 48 kHz, the same on every run.
namespace heldout {
using Signal = std::vector<float>;
constexpr double tau = 2 * 3.14159265358979323846;
static Signal noise(size_t frames, unsigned seed) {
    Signal audio(frames * 2);
    std::mt19937 generator(seed);
    std::uniform_real_distribution<float> value(-.3f, .3f);
    for (auto &sample : audio)
        sample = value(generator);
    return audio;
}
static Signal mono(size_t frames) {
    auto audio = noise(frames, 10);
    for (size_t i = 0; i < frames; ++i)
        audio[i * 2 + 1] = audio[i * 2];
    return audio;
}
static Signal multitone(size_t frames) {
    Signal audio(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double t = double(i) / 48000;
        audio[i * 2] = float(.15 * std::sin(tau * 220 * t) + .1 * std::sin(tau * 1375 * t) + .05 * std::sin(tau * 7040 * t));
        audio[i * 2 + 1] =
            float(.12 * std::sin(tau * 330 * t + 1) + .1 * std::sin(tau * 2489 * t) + .04 * std::sin(tau * 11025 * t));
    }
    return audio;
}
// Decaying clicks with digital silence between them, at different times on each side.
static Signal transients(size_t frames) {
    Signal audio(frames * 2, 0.f);
    for (size_t ch = 0; ch < 2; ++ch)
        for (size_t at = 1000 + ch * 3337; at < frames; at += 9000 + ch * 1234)
            for (size_t i = 0; i < 2000 && at + i < frames; ++i)
                audio[(at + i) * 2 + ch] = float((i % 2 ? -.8 : .8) * std::exp(-double(i) / 300));
    return audio;
}
// A left tone that stops and starts while the right one plays throughout.
static Signal oneSidedGaps(size_t frames) {
    Signal audio(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double t = double(i) / 48000;
        audio[i * 2] = (i / 7000) % 3 == 1 ? 0.f : float(.25 * std::sin(tau * 440 * t));
        audio[i * 2 + 1] = float(.2 * std::sin(tau * 523 * t));
    }
    return audio;
}
// A constant on the left and the highest frequency (+-0.2 each frame) on the right: the two bins
// whose imaginary part is exactly zero.
static Signal dcAndNyquist(size_t frames) {
    Signal audio(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        audio[i * 2] = .2f;
        audio[i * 2 + 1] = i % 2 ? -.2f : .2f;
    }
    return audio;
}
// Identical channels for the first half, as centred SFX are, then different ones.
static Signal monoThenStereo(size_t frames) {
    auto audio = noise(frames, 11);
    for (size_t i = 0; i < frames / 2; ++i)
        audio[i * 2 + 1] = audio[i * 2];
    return audio;
}
// Spans of -0, denormals, levels at the silence gate, identical channels, and samples the stage
// must scrub (NaN and 2e6).
static Signal edgeValues(size_t frames) {
    auto audio = noise(frames, 12);
    for (size_t i = 0; i < frames; ++i) {
        auto &left = audio[i * 2], &right = audio[i * 2 + 1];
        switch ((i / 3000) % 6) {
        case 0: left = -0.f; break;
        case 1: left = right = 1e-40f; break;
        case 2: left *= 2e-5f; break;
        case 3:
            if (i % 7 == 0) left = std::numeric_limits<float>::quiet_NaN();
            if (i % 11 == 0) right = 2e6f;
            break;
        case 4: right = left; break;
        default: break;
        }
    }
    return audio;
}
static std::vector<std::pair<const char *, Signal>> suite(size_t frames) {
    return {{"stereo noise", noise(frames, 9)},     {"mono noise", mono(frames)},
            {"multitone", multitone(frames)},       {"transients", transients(frames)},
            {"one-sided gaps", oneSidedGaps(frames)}, {"mono then stereo", monoThenStereo(frames)},
            {"edge values", edgeValues(frames)},    {"DC and Nyquist", dcAndNyquist(frames)}};
}
// Runs a stage over the signal in calls of the given sizes, cycling through them.
template <class Shifter>
static Signal shift(Signal audio, float pitch, uint32_t fft, size_t gateFrames, const std::vector<size_t> &calls) {
    Shifter shifter(gateFrames);
    for (size_t at = 0, call = 0; at < audio.size() / 2; ++call) {
        const size_t count = std::min(calls[call % calls.size()], audio.size() / 2 - at);
        shifter.process(audio.data() + at * 2, count, pitch, fft);
        at += count;
    }
    return audio;
}
// Samples that differ; +0 and -0 count as the same sample.
struct Difference {
    size_t samples = 0;
    double max = 0;
    void add(const Signal &a, const Signal &b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i])) {
                ++samples;
                max = std::max(max, std::abs(double(a[i]) - b[i]));
            }
    }
};
const std::vector<std::vector<size_t>> callPatterns{{480}, {256}, {64, 113, 256, 480, 31, 1}};
// Stage lists a chain goes through: added, pitch changed, removed, back, moved to another FFT,
// replaced by another instance in the same slot.
template <class Chain> static Signal chain(Signal audio) {
    Chain stages(true, 480);
    const PitchStage a{.75f, 1024, 0, 71}, b{1.25f, 2048, 0, 72}, bFaster{1.5f, 2048, 0, 72}, aWider{.75f, 4096, 0, 71},
        narrow{.83f, 256, 0, 73}, narrowReplaced{.83f, 256, 0, 74}, mid{1.37f, 512, 0, 75};
    const std::vector<std::vector<PitchStage>> lists{{a},    {a, b},      {a, bFaster},  {a},
                                                     {a, b}, {aWider, b}, {narrow, mid}, {narrowReplaced, mid},
                                                     {b},    {}};
    const size_t frames = audio.size() / 2, span = frames / lists.size();
    for (size_t at = 0; at < frames; at += 480) {
        const auto &list = lists[std::min(at / span, lists.size() - 1)];
        stages.process(audio.data() + at * 2, std::min<size_t>(480, frames - at), list.data(), uint8_t(list.size()));
    }
    return audio;
}
}
