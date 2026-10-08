#pragma once
#include <algorithm>
#include <cstdint>

namespace separate_song {
// Nanoseconds spanned by a frame count. Multiplying first would overflow 64 bits past about
// 4.4 days of frames at 48 kHz, and FMOD's DSP clock counts frames from GD's start.
inline uint64_t framesToNs(uint64_t frames, uint32_t rate) {
    return frames / rate * 1000000000 + frames % rate * 1000000000 / rate;
}
// Wall-clock timestamps for the effects tap's mixer blocks. Timestamps follow the frame count, so
// blocks butt up against each other and the OBS plugin can play them back to back. Each block
// steers the timeline at most one sample towards the wall clock, which follows the output
// device's clock drifting from the system clock without jumps. Only a stall starts over.
struct TapClock {
    uint64_t start = 0, frames = 0;
    // Timestamp of the block about to be processed. Call advance() with its length afterwards.
    uint64_t stamp(uint64_t wall, uint32_t rate) {
        auto behind = int64_t(wall - (start + framesToNs(frames, rate)));
        if (!start || behind > 20000000 || behind < -100000000) {
            start = wall;
            frames = 0;
            return wall;
        }
        int64_t sample = 1000000000 / rate;
        start = uint64_t(int64_t(start) + std::clamp(behind / 256, -sample, sample));
        return start + framesToNs(frames, rate);
    }
    void advance(uint32_t count) { frames += count; }
};
} // namespace separate_song
