#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

// A stereo phase-vocoder pitch shifter, the kind FMOD's FMOD_DSP_TYPE_PITCHSHIFT is: it changes pitch
// without changing speed, using frames of fftSize samples that overlap four times. OBS uses it to play
// what an FMOD pitch shifter another mod added in game does to GD's output. Like FMOD's, it delays the
// audio by fftSize - fftSize / 4 samples, so OBS stays as far behind as the game is.
class PitchShifter {
    static constexpr size_t overlap = 4;
    static constexpr double pi = 3.14159265358979323846;
    struct Channel {
        std::vector<float> input, output, accumulator, lastPhase, phaseSum;
    };
    size_t size = 0, step = 0, latency = 0, cursor = 0;
    std::array<Channel, 2> channels;
    std::vector<float> window, magnitude, frequency, shiftedMagnitude, shiftedFrequency;
    std::vector<std::complex<float>> spectrum, twiddles;
    std::vector<size_t> reversed;

    void configure(size_t fftSize) {
        size = fftSize;
        step = size / overlap;
        latency = size - step;
        cursor = latency;
        window.resize(size);
        for (size_t i = 0; i < size; ++i)
            window[i] = float(.5 - .5 * std::cos(2 * pi * double(i) / double(size)));
        for (auto vector : {&magnitude, &frequency, &shiftedMagnitude, &shiftedFrequency})
            vector->assign(size / 2 + 1, 0.f);
        spectrum.assign(size, {});
        twiddles.resize(size / 2);
        for (size_t i = 0; i < size / 2; ++i)
            twiddles[i] = std::polar(1.f, float(-2 * pi * double(i) / double(size)));
        reversed.resize(size);
        unsigned bits = 0;
        while ((size_t(1) << bits) < size)
            ++bits;
        for (size_t i = 0; i < size; ++i) {
            size_t r = 0;
            for (unsigned b = 0; b < bits; ++b)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            reversed[i] = r;
        }
        for (auto &c : channels) {
            c.input.assign(size, 0.f);
            c.output.assign(size, 0.f);
            c.accumulator.assign(size * 2, 0.f);
            c.lastPhase.assign(size / 2 + 1, 0.f);
            c.phaseSum.assign(size / 2 + 1, 0.f);
        }
    }
    // In-place radix-2 FFT; the inverse is unscaled.
    void transform(bool inverse) {
        for (size_t i = 0; i < size; ++i)
            if (i < reversed[i])
                std::swap(spectrum[i], spectrum[reversed[i]]);
        for (size_t length = 2; length <= size; length *= 2) {
            size_t stride = size / length;
            for (size_t start = 0; start < size; start += length)
                for (size_t k = 0; k < length / 2; ++k) {
                    auto w = twiddles[k * stride];
                    if (inverse)
                        w = std::conj(w);
                    auto a = spectrum[start + k], b = spectrum[start + k + length / 2] * w;
                    spectrum[start + k] = a + b;
                    spectrum[start + k + length / 2] = a - b;
                }
        }
    }
    void processFrame(Channel &c, float pitch) {
        const double expected = 2 * pi * double(step) / double(size);
        const size_t bins = size / 2;
        for (size_t k = 0; k < size; ++k)
            spectrum[k] = {c.input[k] * window[k], 0.f};
        transform(false);
        // Analysis: each bin's magnitude and true frequency (in bins) from its phase advance.
        for (size_t k = 0; k <= bins; ++k) {
            float phase = std::arg(spectrum[k]);
            magnitude[k] = 2 * std::abs(spectrum[k]);
            double delta = double(phase) - c.lastPhase[k] - double(k) * expected;
            c.lastPhase[k] = phase;
            delta -= 2 * pi * std::round(delta / (2 * pi));
            frequency[k] = float(double(k) + delta * double(overlap) / (2 * pi));
        }
        std::fill(shiftedMagnitude.begin(), shiftedMagnitude.end(), 0.f);
        std::fill(shiftedFrequency.begin(), shiftedFrequency.end(), 0.f);
        // Moving bins merges or spreads them, which would make the result louder or quieter than the
        // input; scale it back to the energy of the bins that stay in range.
        double kept = 0, moved = 0;
        for (size_t k = 0; k <= bins; ++k) {
            size_t target = size_t(double(k) * pitch);
            if (target > bins)
                break;
            kept += double(magnitude[k]) * magnitude[k];
            shiftedMagnitude[target] += magnitude[k];
            shiftedFrequency[target] = frequency[k] * pitch;
        }
        for (size_t k = 0; k <= bins; ++k)
            moved += double(shiftedMagnitude[k]) * shiftedMagnitude[k];
        const float gain = moved > 0 ? float(std::sqrt(kept / moved)) : 0.f;
        // Synthesis: accumulate each bin's phase at its new frequency.
        for (size_t k = 0; k <= bins; ++k) {
            double delta = (double(shiftedFrequency[k]) - double(k)) * 2 * pi / double(overlap) + double(k) * expected;
            c.phaseSum[k] = float(std::remainder(double(c.phaseSum[k]) + delta, 2 * pi));
            spectrum[k] = std::polar(shiftedMagnitude[k] * gain, c.phaseSum[k]);
        }
        for (size_t k = bins + 1; k < size; ++k)
            spectrum[k] = {};
        transform(true);
        const float scale = 2.f / float(bins * overlap);
        for (size_t k = 0; k < size; ++k)
            c.accumulator[k] += scale * window[k] * spectrum[k].real();
        std::copy_n(c.accumulator.begin(), step, c.output.begin());
        std::copy(c.accumulator.begin() + step, c.accumulator.end(), c.accumulator.begin());
        std::fill(c.accumulator.end() - step, c.accumulator.end(), 0.f);
        std::copy(c.input.begin() + step, c.input.end(), c.input.begin());
    }

  public:
    bool active() const { return size != 0; }
    // Shifts `frames` interleaved stereo frames in place by `pitch` (0.5 is an octave down) with frames
    // of fftSize samples. A pitch of 1 passes audio through untouched and forgets the shifter's
    // state, as removing FMOD's DSP would.
    void process(float *audio, size_t frames, float pitch, uint32_t fftSize) {
        if (!std::isfinite(pitch) || std::abs(pitch - 1.f) < 1e-4f) {
            if (size) {
                size = 0;
                for (auto &c : channels)
                    c = {};
            }
            return;
        }
        size_t wanted = 256;
        while (wanted < std::clamp<uint32_t>(fftSize ? fftSize : 1024, 256, 4096))
            wanted *= 2;
        if (wanted != size)
            configure(wanted);
        pitch = std::clamp(pitch, .25f, 4.f);
        for (size_t i = 0; i < frames; ++i) {
            for (size_t ch = 0; ch < 2; ++ch) {
                auto &c = channels[ch];
                c.input[cursor] = audio[i * 2 + ch];
                audio[i * 2 + ch] = c.output[cursor - latency];
            }
            if (++cursor >= size) {
                cursor = latency;
                for (auto &c : channels)
                    processFrame(c, pitch);
            }
        }
    }
};
