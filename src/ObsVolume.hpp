#pragma once

#include <Geode/loader/Mod.hpp>
#include <algorithm>
#include <cmath>

namespace separate_song::obs_volume {
inline float sanitize(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 1.f;
}
inline float music() {
    return sanitize(geode::Mod::get()->getSavedValue<float>("obs-music-volume", 1.f));
}
inline float effects() {
    return sanitize(geode::Mod::get()->getSavedValue<float>("obs-effects-volume", 1.f));
}
inline void setMusic(float value) {
    geode::Mod::get()->setSavedValue("obs-music-volume", sanitize(value));
}
inline void setEffects(float value) {
    geode::Mod::get()->setSavedValue("obs-effects-volume", sanitize(value));
}
}
