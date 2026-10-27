#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// FMOD's FMOD_DSP_TYPE_PITCHSHIFT is Stephan M. Bernsee's smbPitchShift: a phase vocoder that
// changes pitch without changing speed, using frames of fftSize samples that overlap four times.
// OBS applies the same processing to the audio it plays so another mod's pitch shifter sounds in
// OBS as it does in game. The differences from the published code, measured from GD's fmod.dll
// 2.02.31, are that the bin shift rounds instead of truncating and that a silent block is skipped
// without moving the stage's state at all.
//
// This file is a derivative of smbPitchShift.cpp v1.2:
//
// COPYRIGHT 1999-2015 Stephan M. Bernsee <s.bernsee [AT] zynaptiq [DOT] com>
//
// The Wide Open License (WOL)
//
// Permission to use, copy, modify, distribute and sell this software and its documentation for any
// purpose is hereby granted without fee, provided that the above copyright notice and this license
// appear in all source copies. THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED
// WARRANTY OF ANY KIND. See http://www.dspguru.com/wol.htm for more information.
class PitchShifter {
    static constexpr size_t overlap = 4, maxFft = 4096, maxBins = maxFft / 2 + 1;
    static constexpr double pi = 3.14159265358979323846;
    struct Tables {
        size_t size = 0, step = 0, latency = 0;
        std::vector<float> window;
        std::vector<std::complex<float>> twiddles, inverseTwiddles;
        std::vector<size_t> reversed;
    };
    // Built once for every size FMOD accepts; a static here is initialized once and safely shared.
    static const std::array<Tables, 5> &allTables() {
        static const std::array<Tables, 5> tables = [] {
            std::array<Tables, 5> built;
            const size_t sizes[5] = {256, 512, 1024, 2048, 4096};
            for (size_t i = 0; i < 5; ++i) {
                const size_t size = sizes[i];
                auto &t = built[i];
                t.size = size;
                t.step = size / overlap;
                t.latency = size - t.step;
                t.window.resize(size);
                for (size_t k = 0; k < size; ++k)
                    t.window[k] = float(.5 - .5 * std::cos(2 * pi * double(k) / double(size)));
                t.twiddles.resize(size / 2);
                t.inverseTwiddles.resize(size / 2);
                for (size_t k = 0; k < size / 2; ++k) {
                    t.twiddles[k] = std::polar(1.f, float(-2 * pi * double(k) / double(size)));
                    t.inverseTwiddles[k] = std::conj(t.twiddles[k]);
                }
                t.reversed.resize(size);
                unsigned bits = 0;
                while ((size_t(1) << bits) < size)
                    ++bits;
                for (size_t k = 0; k < size; ++k) {
                    size_t r = 0;
                    for (unsigned b = 0; b < bits; ++b)
                        r |= ((k >> b) & 1) << (bits - 1 - b);
                    t.reversed[k] = r;
                }
            }
            return built;
        }();
        return tables;
    }
    static size_t normalizeFft(uint32_t fftSize) {
        size_t size = 256;
        const auto wanted = std::clamp<uint32_t>(fftSize ? fftSize : 1024, 256, 4096);
        while (size < wanted)
            size *= 2;
        return size;
    }
    struct Channel {
        std::array<float, maxFft> input{}, output{};
        std::array<float, maxFft * 2> accumulator{};
        std::array<float, maxBins> lastPhase{}, phaseSum{};
        size_t cursor = 0;
        bool primed = false;
    };
    size_t gateFrames = 512, size = 0;
    const Tables *tables = nullptr;
    std::array<Channel, 2> channels;
    std::array<float, maxBins> magnitude{}, frequency{}, shiftedMagnitude{}, shiftedFrequency{};
    std::array<std::complex<float>, maxFft> spectrum{};

    // In-place radix-2 FFT; the inverse is unscaled, as in Bernsee's code.
    void transform(bool inverse) {
        const auto &t = *tables;
        for (size_t i = 0; i < t.size; ++i)
            if (i < t.reversed[i])
                std::swap(spectrum[i], spectrum[t.reversed[i]]);
        for (size_t length = 2; length <= t.size; length *= 2) {
            const size_t stride = t.size / length;
            const auto &twiddles = inverse ? t.inverseTwiddles : t.twiddles;
            for (size_t start = 0; start < t.size; start += length)
                for (size_t k = 0; k < length / 2; ++k) {
                    const auto a = spectrum[start + k];
                    const auto b = spectrum[start + k + length / 2] * twiddles[k * stride];
                    spectrum[start + k] = a + b;
                    spectrum[start + k + length / 2] = a - b;
                }
        }
    }
    void processFrame(Channel &c, float pitch) {
        const auto &t = *tables;
        const size_t bins = t.size / 2;
        const double expected = 2 * pi * double(t.step) / double(t.size);
        for (size_t k = 0; k < t.size; ++k)
            spectrum[k] = {c.input[k] * t.window[k], 0.f};
        transform(false);
        // Analysis: each bin's magnitude and true frequency (in bins) from its phase advance.
        for (size_t k = 0; k <= bins; ++k) {
            const float phase = std::arg(spectrum[k]);
            magnitude[k] = 2 * std::abs(spectrum[k]);
            double delta = double(phase) - double(c.lastPhase[k]) - double(k) * expected;
            c.lastPhase[k] = phase;
            delta -= 2 * pi * std::round(delta / (2 * pi));
            frequency[k] = float(double(k) + delta * double(overlap) / (2 * pi));
        }
        shiftedMagnitude.fill(0.f);
        shiftedFrequency.fill(0.f);
        // FMOD rounds the destination bin; Bernsee's code truncates, which does not sound the same.
        for (size_t k = 0; k <= bins; ++k) {
            const size_t target = size_t(float(k) * pitch + .5f);
            if (target > bins)
                break;
            shiftedMagnitude[target] += magnitude[k];
            shiftedFrequency[target] = frequency[k] * pitch;
        }
        // Synthesis: accumulate each bin's phase at its new frequency, wrapped each hop as FMOD does.
        for (size_t k = 0; k <= bins; ++k) {
            const double delta = (double(shiftedFrequency[k]) - double(k)) * 2 * pi / double(overlap) +
                                 double(k) * expected;
            double phase = double(c.phaseSum[k]) + delta;
            if (phase > pi || phase < -pi)
                phase -= double(int(phase / (2 * pi))) * 2 * pi;
            c.phaseSum[k] = float(phase);
            spectrum[k] = std::polar(shiftedMagnitude[k], float(phase));
        }
        for (size_t k = bins + 1; k < t.size; ++k)
            spectrum[k] = {};
        transform(true);
        const float scale = 2.f / float(bins * overlap);
        for (size_t k = 0; k < t.size; ++k)
            c.accumulator[k] += scale * t.window[k] * spectrum[k].real();
        std::copy_n(c.accumulator.begin(), t.step, c.output.begin());
        std::copy(c.accumulator.begin() + t.step, c.accumulator.begin() + t.size, c.accumulator.begin());
        std::fill(c.accumulator.begin() + t.size - t.step, c.accumulator.begin() + t.size, 0.f);
        std::copy(c.input.begin() + t.step, c.input.begin() + t.size, c.input.begin());
    }

  public:
    explicit PitchShifter(size_t gateFrames = 512) : gateFrames(gateFrames ? gateFrames : 512) {}
    // Builds the shared tables. Call before any audio thread starts; process() would build them
    // lazily on first use otherwise.
    static void prepareTables() { (void)allTables(); }
    bool active() const { return size != 0; }
    // Forgets the stage's audio and phase without freeing its fixed buffers.
    void clear() {
        for (auto &c : channels)
            c = {};
        size = 0;
        tables = nullptr;
    }
    void reset(uint32_t fftSize) {
        clear();
        size = normalizeFft(fftSize);
        size_t index = 0;
        for (size_t smaller = 256; smaller < size; smaller *= 2)
            ++index;
        tables = &allTables()[index];
    }
    // Shifts `frames` interleaved stereo frames in place. `gateFrames` is FMOD's mixing block: a
    // channel whose block sums below 0.001 (scaled for a shorter call) outputs silence and keeps
    // its state, so a pause or a gap holds the audio still inside the stage.
    void process(float *audio, size_t frames, float pitch, uint32_t fftSize) {
        if (!audio || !frames)
            return;
        if (!std::isfinite(pitch)) {
            clear();
            return;
        }
        if (normalizeFft(fftSize) != size)
            reset(fftSize);
        pitch = std::clamp(pitch, .5f, 2.f);
        const double threshold = .001 * double(frames) / double(gateFrames);
        for (size_t ch = 0; ch < 2; ++ch) {
            double sum = 0;
            for (size_t i = 0; i < frames; ++i) {
                // A corrupt float WAV must not poison this stage's retained FFT/phase history.
                auto &sample = audio[i * 2 + ch];
                if (!std::isfinite(sample))
                    sample = 0;
                sum += std::abs(sample);
            }
            if (sum < threshold) {
                for (size_t i = 0; i < frames; ++i)
                    audio[i * 2 + ch] = 0;
                continue;
            }
            auto &c = channels[ch];
            if (!c.primed) {
                c.primed = true;
                c.cursor = tables->latency;
            }
            for (size_t i = 0; i < frames; ++i) {
                c.input[c.cursor] = audio[i * 2 + ch];
                audio[i * 2 + ch] = c.output[c.cursor - tables->latency];
                if (++c.cursor >= tables->size) {
                    c.cursor = tables->latency;
                    processFrame(c, pitch);
                }
            }
        }
    }
};

// The ordered pitch stages of one FMOD object (a channel, a music group, a bus). Each slot keeps the
// DSP instance and FFT size it was built for: a changed instance or a stage that returns after being
// absent starts fresh, while a pitch change keeps the audio it is holding. A DSP the mod destroys and
// recreates at the same address between two samples is indistinguishable and is documented as a limit.
// The effects chain is built with `preallocate` before its audio thread starts; decoder-side chains
// build a slot the first time a packet names it. A list longer than eight keeps the first eight, as
// the wire and the mod's observer do.
class PitchChain {
    struct Slot {
        std::unique_ptr<PitchShifter> shifter;
        uint64_t instance = 0;
        uint32_t fft = 0;
        bool present = false;
    };
    std::array<Slot, 8> slots;
    size_t gateFrames;

  public:
    explicit PitchChain(bool preallocate = false, size_t gateFrames = 512) : gateFrames(gateFrames) {
        if (preallocate)
            for (auto &slot : slots)
                slot.shifter = std::make_unique<PitchShifter>(gateFrames);
    }
    void reset() {
        for (auto &slot : slots) {
            if (slot.shifter)
                slot.shifter->clear();
            slot.instance = 0;
            slot.fft = 0;
            slot.present = false;
        }
    }
    void process(float *stereo, size_t frames, const PitchStage *stages, uint8_t count) {
        const size_t used = std::min<size_t>(count, slots.size());
        if (!stereo || (!stages && used))
            return;
        for (size_t i = 0; i < slots.size(); ++i) {
            auto &slot = slots[i];
            if (i >= used) {
                slot.present = false;
                continue;
            }
            if (!slot.shifter)
                slot.shifter = std::make_unique<PitchShifter>(gateFrames);
            const auto &stage = stages[i];
            if (!slot.present || slot.instance != stage.instance || slot.fft != stage.fft) {
                slot.shifter->reset(stage.fft);
                slot.instance = stage.instance;
                slot.fft = stage.fft;
            }
            slot.present = true;
        }
        for (size_t i = 0; i < used; ++i)
            slots[i].shifter->process(stereo, frames, stages[i].pitch, stages[i].fft);
    }
};
