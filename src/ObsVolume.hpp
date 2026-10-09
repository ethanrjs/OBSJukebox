#pragma once

#include <Geode/loader/Mod.hpp>
#include <algorithm>
#include <cmath>

namespace separate_song::obs_volume {
inline float sanitize(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 1.f;
}
inline float& musicCache() {
    static float value = sanitize(geode::Mod::get()->getSavedValue<float>("obs-music-volume", 1.f));
    return value;
}
inline float& effectsCache() {
    static float value = sanitize(geode::Mod::get()->getSavedValue<float>("obs-effects-volume", 1.f));
    return value;
}
inline float music() { return musicCache(); }
inline float effects() { return effectsCache(); }
inline void setMusic(float value) {
    musicCache() = sanitize(value);
    geode::Mod::get()->setSavedValue("obs-music-volume", musicCache());
}
inline void setEffects(float value) {
    effectsCache() = sanitize(value);
    geode::Mod::get()->setSavedValue("obs-effects-volume", effectsCache());
}
}
