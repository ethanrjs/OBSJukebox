#pragma once
#include "LinkPacket.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace separate_song {
// What main.cpp reads back from one FMOD DSP. `usable` is false when a query failed or the DSP is
// bypassed or inactive, since FMOD does not process those.
struct ObservedDsp {
    int type = 0;
    float pitch = 1.f;
    float fft = 0.f;
    bool usable = false;
    uint64_t instance = 0;
};
struct StageList {
    std::array<PitchStage, 8> stages{};
    uint8_t count = 0;
    bool overflow = false;
};
// The pitch shifters another mod put on a path, in the order they process audio. Pure: no FMOD or
// Geode types, so it can be tested without either.
inline StageList pitchStages(std::span<const ObservedDsp> dsps) {
    constexpr int pitchShiftType = 13; // FMOD_DSP_TYPE_PITCHSHIFT, kept numeric so this stays pure
    StageList list;
    for (const auto &dsp : dsps) {
        if (dsp.type != pitchShiftType || !dsp.usable || !std::isfinite(dsp.pitch) ||
            !std::isfinite(dsp.fft) || dsp.instance == 0)
            continue;
        if (dsp.fft != 256 && dsp.fft != 512 && dsp.fft != 1024 && dsp.fft != 2048 && dsp.fft != 4096)
            continue;
        if (list.count == list.stages.size()) {
            list.overflow = true;
            continue;
        }
        auto &stage = list.stages[list.count++];
        stage.pitch = std::clamp(dsp.pitch, .5f, 2.f);
        stage.fft = uint16_t(dsp.fft);
        stage.instance = dsp.instance;
    }
    return list;
}
} // namespace separate_song
