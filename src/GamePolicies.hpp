#pragma once
#include "MusicSampling.hpp"
#include <algorithm>
#include <span>

namespace separate_song {
// Stable ordering makes the last FMOD point at a timestamp authoritative.
inline void normalizeMusicFades(std::vector<MusicFade>& points) {
    std::stable_sort(points.begin(), points.end(),
        [](const MusicFade& a, const MusicFade& b) { return a.clock < b.clock; });
    size_t written = 0;
    for (auto point : points) {
        point.gain = finiteMusicGain(point.gain);
        if (written && points[written - 1].clock == point.clock) points[written - 1] = point;
        else points[written++] = point;
    }
    points.resize(written);
}
// Applies after scheduled-stop points are appended as they may collide with fades.
template<class Point, size_t Extent>
inline unsigned normalizePacketFades(std::span<Point, Extent> points, unsigned count) {
    count = std::min(count, static_cast<unsigned>(points.size()));
    std::stable_sort(points.begin(), points.begin() + count,
        [](const Point& a, const Point& b) { return a.offsetNs < b.offsetNs; });
    unsigned written = 0;
    for (unsigned i = 0; i < count; ++i) {
        auto point = points[i];
        if (!point.offsetNs || point.offsetNs > 86400000000000ULL) continue;
        point.gain = finiteMusicGain(point.gain);
        if (written && points[written - 1].offsetNs == point.offsetNs) points[written - 1] = point;
        else points[written++] = point;
    }
    return written;
}
}
