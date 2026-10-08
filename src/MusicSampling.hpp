#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace separate_song {
struct MusicFade { uint64_t clock; float gain; };
inline float finiteMusicGain(float gain) {
    return std::isfinite(gain) ? std::clamp(gain, 0.f, 4.f) : 0.f;
}
inline float backgroundMusicGain(float start, float duration, float elapsed) {
    if (start == 0.f || duration <= 0.f) return 1.f;
    if (start < 0.f) return 0.f;
    return std::clamp((elapsed - start) / duration, 0.f, 1.f);
}
inline float musicFadeAt(std::span<const MusicFade> points, uint64_t clock) {
    if (points.empty()) return 1.f;
    if (clock <= points.front().clock) return finiteMusicGain(points.front().gain);
    for (size_t i = 1; i < points.size(); ++i) {
        if (clock > points[i].clock) continue;
        auto& a = points[i - 1]; auto& b = points[i];
        if (b.clock <= a.clock) return finiteMusicGain(b.gain);
        double fraction = double(clock - a.clock) / double(b.clock - a.clock);
        return finiteMusicGain(float(a.gain + (b.gain - a.gain) * fraction));
    }
    return finiteMusicGain(points.back().gain);
}
inline bool musicPositionJump(double previous, double current, double elapsed, double rate) {
    return std::abs(current - (previous + elapsed * rate)) > .075;
}
inline bool canContinueMusicEOF(double position, double duration, double elapsed,
                                double rate, bool loop, bool scheduledStop) {
    return !loop && !scheduledStop && duration > 0 && elapsed >= 0 && elapsed < .25 &&
        position + elapsed * rate >= duration - .05;
}
}
