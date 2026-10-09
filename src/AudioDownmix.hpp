#pragma once
#include <array>

namespace separate_song {
// FMOD's standard interleaved speaker order is L R C LFE surround-L
// surround-R back-L back-R. Mono and quad have their own layouts.
inline std::array<float, 2> downmixStereo(const float *frame, int channels) {
    if (channels <= 0)
        return {0, 0};
    if (channels == 1)
        return {frame[0], frame[0]};
    float left = frame[0], right = frame[1];
    constexpr float surroundGain = .70710678f;
    if (channels == 4) {
        left += frame[2] * surroundGain;
        right += frame[3] * surroundGain;
    } else if (channels >= 3) {
        left += frame[2] * surroundGain;
        right += frame[2] * surroundGain;
        int surround = channels == 5 ? 3 : 4;
        // LFE is deliberately omitted, as in a conventional stereo fold-down.
        for (int channel = surround; channel < channels; ++channel) {
            if ((channel - surround) % 2 == 0)
                left += frame[channel] * surroundGain;
            else
                right += frame[channel] * surroundGain;
        }
    }
    return {left, right};
}
} // namespace separate_song
