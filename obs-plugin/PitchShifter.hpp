#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// FMOD's FMOD_DSP_TYPE_PITCHSHIFT is Stephan M. Bernsee's smbPitchShift phase vocoder; this mirrors
// fmod.dll 2.02.31 so another mod's pitch shifter sounds in OBS as it does in game.

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

// AVX versions of PitchShifter's FFT loops: eight lanes of the same statements, no FMA, so each
// element gets the same IEEE operations in the same order. available() checks AVX and OS state.
namespace pitch_simd {
bool available();
// PitchShifter::butterflies for a `half` that is a multiple of 8.
void butterflies(
    float* lowRe, float* lowIm, float* highRe, float* highIm, size_t half, float const* cosines,
    float const* sines
);
}

class PitchShifter {
    static constexpr size_t overlap = 4, maxFft = 4096, maxBins = maxFft / 2 + 1;
    static constexpr double pi = 3.14159265358979323846;

    struct Tables {
        size_t size = 0, step = 0, latency = 0;
        std::vector<float> window;
        // Twiddle factors in the order each pass uses them; the pass that combines halves of
        // `half` points starts at index half - 1.
        std::vector<float> cosines, sines, inverseSines;
        std::vector<size_t> reversed;
    };

    // Built once for every size FMOD accepts; a static here is initialized once and safely shared.
    static std::array<Tables, 5> const& allTables() {
        static std::array<Tables, 5> const tables = [] {
            std::array<Tables, 5> built;
            size_t const sizes[5] = {256, 512, 1024, 2048, 4096};
            for (size_t i = 0; i < 5; ++i) {
                size_t const size = sizes[i];
                auto& t = built[i];
                t.size = size;
                t.step = size / overlap;
                t.latency = size - t.step;
                t.window.resize(size);
                for (size_t k = 0; k < size; ++k)
                    t.window[k] = float(.5 - .5 * std::cos(2 * pi * double(k) / double(size)));
                for (size_t half = 1; half < size; half *= 2)
                    for (size_t k = 0; k < half; ++k) {
                        auto const twiddle = std::polar(
                            1.f, float(-2 * pi * double(k * size / (half * 2)) / double(size))
                        );
                        t.cosines.push_back(twiddle.real());
                        t.sines.push_back(twiddle.imag());
                        t.inverseSines.push_back(std::conj(twiddle).imag());
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
        auto const wanted = std::clamp<uint32_t>(fftSize ? fftSize : 1024, 256, 4096);
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
    Tables const* tables = nullptr;
    std::array<Channel, 2> channels;
    std::array<float, maxBins> phase{}, frequency{}, shiftedMagnitude{}, shiftedFrequency{};
    std::array<float, maxFft> real{}, imaginary{};

    // Combines `half` pairs: low a and high b become a + bw and a - bw. Rounds as
    // std::complex<float>: each product its own statement, so clang cannot fuse it into an FMA.
    static void butterflies(
        float* __restrict lowRe, float* __restrict lowIm, float* __restrict highRe,
        float* __restrict highIm, size_t half, float const* __restrict cosines,
        float const* __restrict sines
    ) {
        for (size_t k = 0; k < half; ++k) {
            float const ac = highRe[k] * cosines[k], bd = highIm[k] * sines[k];
            float const ad = highRe[k] * sines[k], bc = highIm[k] * cosines[k];
            float const br = ac - bd, bi = ad + bc;
            highRe[k] = lowRe[k] - br;
            highIm[k] = lowIm[k] - bi;
            lowRe[k] = lowRe[k] + br;
            lowIm[k] = lowIm[k] + bi;
        }
    }

    // The forward FFT's last pass without the bins above N/2, which no one reads. Same operations
    // as butterflies() for what it keeps.
    static void lowButterflies(
        float* __restrict lowRe, float* __restrict lowIm, float const* __restrict highRe,
        float const* __restrict highIm, size_t half, float const* __restrict cosines,
        float const* __restrict sines
    ) {
        for (size_t k = 0; k < half; ++k) {
            float const ac = highRe[k] * cosines[k], bd = highIm[k] * sines[k];
            float const ad = highRe[k] * sines[k], bc = highIm[k] * cosines[k];
            float const br = ac - bd, bi = ad + bc;
            lowRe[k] = lowRe[k] + br;
            lowIm[k] = lowIm[k] + bi;
        }
    }

    // The inverse FFT's last pass without imaginary parts: overlap-add reads only the real part.
    static void realButterflies(
        float* __restrict lowRe, float* __restrict highRe, float const* __restrict highIm,
        size_t half, float const* __restrict cosines, float const* __restrict sines
    ) {
        for (size_t k = 0; k < half; ++k) {
            float const ac = highRe[k] * cosines[k], bd = highIm[k] * sines[k];
            float const br = ac - bd;
            highRe[k] = lowRe[k] - br;
            lowRe[k] = lowRe[k] + br;
        }
    }

    // One butterfly in registers, with the same operations as butterflies().
    static void butterfly(
        float& lowRe, float& lowIm, float& highRe, float& highIm, float cosine, float sine
    ) {
        float const ac = highRe * cosine, bd = highIm * sine;
        float const ad = highRe * sine, bc = highIm * cosine;
        float const br = ac - bd, bi = ad + bc;
        highRe = lowRe - br;
        highIm = lowIm - bi;
        lowRe = lowRe + br;
        lowIm = lowIm + bi;
    }

    // Radix-2 FFT on a bit-reversed spectrum, unscaled when inverse as in Bernsee. The first two
    // passes are too short to vectorize, and known +0 inputs make their products constants.
    struct ZeroProducts {
        float sine, cosine;
    };

    static ZeroProducts zeroProducts(float cosine, float sine) {
        return {0.f * sine, 0.f * cosine};
    }

    static void butterflyRealInputs(
        float& lowRe, float& lowIm, float& highRe, float& highIm, float cosine, float sine,
        ZeroProducts zero
    ) {
        float const ac = highRe * cosine, ad = highRe * sine;
        float const br = ac - zero.sine, bi = ad + zero.cosine;
        highRe = lowRe - br;
        highIm = 0.f - bi;
        lowRe = lowRe + br;
        lowIm = 0.f + bi;
    }

    static void butterflyZeroHigh(
        float& lowRe, float& lowIm, float& highRe, float& highIm, ZeroProducts zero
    ) {
        float const br = zero.cosine - zero.sine, bi = zero.sine + zero.cosine;
        highRe = lowRe - br;
        highIm = lowIm - bi;
        lowRe = lowRe + br;
        lowIm = lowIm + bi;
    }

    // Pass 2 and the store, after a pass 1 that the caller does.
    void secondPass(size_t at, float (&re)[4], float (&im)[4], std::vector<float> const& sines) {
        auto const& cosines = tables->cosines;
        butterfly(re[0], im[0], re[2], im[2], cosines[1], sines[1]);
        butterfly(re[1], im[1], re[3], im[3], cosines[2], sines[2]);
        std::copy_n(re, 4, &real[at]);
        std::copy_n(im, 4, &imaginary[at]);
    }

    // Point k of group 4i is reversed[4i] + offsets[k], where bit reversal maps 1, 2, 3 to
    // N/2, N/4, 3N/4.
    std::array<size_t, 4> groupOffsets() const {
        return {0, tables->size / 2, tables->size / 4, tables->size / 4 * 3};
    }

    // The passes between the second and the last, eight lanes wide where AVX runs.
    void middlePasses(std::vector<float> const& sines) {
        auto const& t = *tables;
        bool const wide = !portableOnly && pitch_simd::available();
        for (size_t half = 4; half < t.size / 2; half *= 2)
            for (size_t start = 0; start < t.size; start += half * 2) {
                float *lowRe = &real[start], *lowIm = &imaginary[start];
                float *highRe = &real[start + half], *highIm = &imaginary[start + half];
                if (wide && half >= 8)
                    pitch_simd::butterflies(
                        lowRe, lowIm, highRe, highIm, half, &t.cosines[half - 1], &sines[half - 1]
                    );
                else
                    butterflies(
                        lowRe, lowIm, highRe, highIm, half, &t.cosines[half - 1], &sines[half - 1]
                    );
            }
    }

    // The spectrum of the channel's windowed frame, up to bin N/2. The input is real, so every
    // point starts with a +0 imaginary part.
    void forwardTransform(Channel const& c) {
        auto const& t = *tables;
        auto const offsets = groupOffsets();
        float const cosine = t.cosines[0], sine = t.sines[0];
        auto const zero = zeroProducts(cosine, sine);
        for (size_t at = 0; at < t.size; at += 4) {
            float re[4], im[4];
            for (size_t i = 0; i < 4; ++i) {
                size_t const point = t.reversed[at] + offsets[i];
                re[i] = c.input[point] * t.window[point];
            }
            butterflyRealInputs(re[0], im[0], re[1], im[1], cosine, sine, zero);
            butterflyRealInputs(re[2], im[2], re[3], im[3], cosine, sine, zero);
            secondPass(at, re, im, t.sines);
        }
        middlePasses(t.sines);
        size_t const half = t.size / 2;
        butterfly(
            real[0], imaginary[0], real[half], imaginary[half], t.cosines[half - 1], t.sines[half - 1]
        );
        lowButterflies(
            &real[1],
            &imaginary[1],
            &real[half + 1],
            &imaginary[half + 1],
            half - 1,
            &t.cosines[half],
            &t.sines[half]
        );
    }

    // The real part of the inverse FFT of bins 0..N/2 (shiftedMagnitude at phaseSum). Bins above
    // N/2 are +0, so groups of four have only points 0 and 2 set; the first group also has N/2.
    void inverseTransform(Channel const& c) {
        auto const& t = *tables;
        auto const offsets = groupOffsets();
        // A bin nothing was shifted into stays silent; std::polar would give it a signed zero.
        // That only flips a zero's sign in real[]; adding it to the +0 accumulator never gives -0.
        auto bin = [&](size_t k, float& re, float& im) {
            auto const value = shiftedMagnitude[k] == 0 ?
                std::complex<float>() :
                std::polar(shiftedMagnitude[k], c.phaseSum[k]);
            re = value.real();
            im = value.imag();
        };
        float const cosine = t.cosines[0], sine = t.inverseSines[0];
        auto const zero = zeroProducts(cosine, sine);
        for (size_t at = 0; at < t.size; at += 4) {
            float re[4] = {}, im[4] = {};
            bin(t.reversed[at], re[0], im[0]);
            bin(t.reversed[at] + offsets[2], re[2], im[2]);
            if (at == 0) {
                bin(offsets[1], re[1], im[1]);
                butterfly(re[0], im[0], re[1], im[1], cosine, sine);
            }
            else {
                butterflyZeroHigh(re[0], im[0], re[1], im[1], zero);
            }
            butterflyZeroHigh(re[2], im[2], re[3], im[3], zero);
            secondPass(at, re, im, t.inverseSines);
        }
        middlePasses(t.inverseSines);
        size_t const half = t.size / 2;
        realButterflies(
            &real[0], &real[half], &imaginary[half], half, &t.cosines[half - 1], &t.inverseSines[half - 1]
        );
    }

    // std::round semantics without the CRT call MSVC makes, or the compares and copysign it will
    // not vectorize. Exact for |x| < 2^31; a zero result is +0, which binFrequencies accepts.
    static double roundHalfAway(double x) {
        double const whole = double(int(x));
        double const fraction = x - whole;
        return whole + double(int(2 * fraction));
    }

    // Each bin's true frequency (in bins) from how far its phase moved since the last frame.
    // Separate arrays so the compiler can vectorize it.
    static void binFrequencies(
        float const* __restrict phase, float* __restrict lastPhase, float* __restrict frequency,
        int count, double expected
    ) {
        for (int k = 0; k < count; ++k) {
            double delta = double(phase[k]) - double(lastPhase[k]) - double(k) * expected;
            lastPhase[k] = phase[k];
            delta -= 2 * pi * roundHalfAway(delta / (2 * pi));
            frequency[k] = float(double(k) + delta * double(overlap) / (2 * pi));
        }
    }

    // Moves each bin's phase on by its new frequency, wrapped as FMOD does: outside [-pi, pi] a
    // phase loses its whole turns. Inside it, turns truncate to 0, so no range test is needed.
    static void advancePhases(
        float const* __restrict shiftedFrequency, float* __restrict phaseSum, int count, double expected
    ) {
        for (int k = 0; k < count; ++k) {
            double const delta =
                (double(shiftedFrequency[k]) - double(k)) * 2 * pi / double(overlap) +
                double(k) * expected;
            double const phase = double(phaseSum[k]) + delta;
            phaseSum[k] = float(phase - double(int(phase / (2 * pi))) * 2 * pi);
        }
    }

    void processFrame(Channel& c, float pitch) {
        auto const& t = *tables;
        size_t const bins = t.size / 2;
        double const expected = 2 * pi * double(t.step) / double(t.size);
        forwardTransform(c);
        // The phases come first, so binFrequencies has no calls in it.
        for (size_t k = 0; k <= bins; ++k)
            phase[k] = std::arg(std::complex<float>(real[k], imaginary[k]));
        binFrequencies(phase.data(), c.lastPhase.data(), frequency.data(), int(bins + 1), expected);
        shiftedMagnitude.fill(0.f);
        shiftedFrequency.fill(0.f);
        // FMOD rounds the destination bin; Bernsee truncates, which does not sound the same.
        // Doubling is exact, so fusing it into the sum rounds as the separate steps did.
        for (size_t k = 0; k <= bins; ++k) {
            size_t const target = size_t(float(k) * pitch + .5f);
            if (target > bins) break;
            shiftedMagnitude[target] += 2 * std::abs(std::complex<float>(real[k], imaginary[k]));
            shiftedFrequency[target] = frequency[k] * pitch;
        }
        advancePhases(shiftedFrequency.data(), c.phaseSum.data(), int(bins + 1), expected);
        inverseTransform(c);
        float const scale = 2.f / float(bins * overlap);
        for (size_t k = 0; k < t.size; ++k)
            c.accumulator[k] += scale * t.window[k] * real[k];
        std::copy_n(c.accumulator.begin(), t.step, c.output.begin());
        std::copy(
            c.accumulator.begin() + t.step, c.accumulator.begin() + t.size, c.accumulator.begin()
        );
        std::fill(c.accumulator.begin() + t.size - t.step, c.accumulator.begin() + t.size, 0.f);
        std::copy(c.input.begin() + t.step, c.input.begin() + t.size, c.input.begin());
    }

public:
    // Tests set this to check the AVX loops against the portable ones; leave it false elsewhere.
    bool portableOnly = false;

    explicit PitchShifter(size_t gateFrames = 512) : gateFrames(gateFrames ? gateFrames : 512) {}

    // Builds the shared tables and checks for AVX before any audio thread starts.
    static void prepareTables() {
        (void)allTables();
        (void)pitch_simd::available();
    }

    bool active() const {
        return size != 0;
    }

    // Forgets the stage's audio and phase without freeing its fixed buffers.
    void clear() {
        for (auto& c : channels)
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

    // `gateFrames` is FMOD's mixing block: a channel whose block sums below 0.001, scaled for a
    // shorter call, outputs silence and keeps its state, so a pause holds the audio in the stage.
    void process(float* audio, size_t frames, float pitch, uint32_t fftSize) {
        if (!audio || !frames) return;
        if (!std::isfinite(pitch)) {
            clear();
            return;
        }
        if (normalizeFft(fftSize) != size) reset(fftSize);
        pitch = std::clamp(pitch, .5f, 2.f);
        double const threshold = .001 * double(frames) / double(gateFrames);
        for (size_t ch = 0; ch < 2; ++ch) {
            double sum = 0;
            for (size_t i = 0; i < frames; ++i) {
                // A corrupt float WAV must not poison the retained FFT/phase state: NaN, Inf, or
                // a sample that overflows the FFT. 1e6 is 120 dB above full scale.
                auto& sample = audio[i * 2 + ch];
                if (!(std::abs(sample) <= 1e6f)) sample = 0;
                sum += std::abs(sample);
            }
            if (sum < threshold) {
                for (size_t i = 0; i < frames; ++i)
                    audio[i * 2 + ch] = 0;
                continue;
            }
            auto& c = channels[ch];
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

// Ordered pitch stages of one FMOD object; a pitch change keeps the audio a stage holds, and a DSP
// recreated at the same address between samples is indistinguishable, a documented limit.
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
    explicit PitchChain(bool preallocate = false, size_t gateFrames = 512) :
        gateFrames(gateFrames) {
        if (preallocate)
            for (auto& slot : slots)
                slot.shifter = std::make_unique<PitchShifter>(gateFrames);
    }

    void reset() {
        for (auto& slot : slots) {
            if (slot.shifter) slot.shifter->clear();
            slot.instance = 0;
            slot.fft = 0;
            slot.present = false;
        }
    }

    void process(float* stereo, size_t frames, PitchStage const* stages, uint8_t count) {
        size_t const used = std::min<size_t>(count, slots.size());
        if (!stereo || (!stages && used)) return;
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (i >= used) {
                slot.present = false;
                continue;
            }
            if (!slot.shifter) slot.shifter = std::make_unique<PitchShifter>(gateFrames);
            auto const& stage = stages[i];
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
