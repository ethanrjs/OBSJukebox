#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include "ObsVolume.hpp"

using namespace geode::prelude;

namespace {
class EffectsCreationGuard {
    float& volume;
    float saved;
public:
    EffectsCreationGuard(float& value, bool capture) : volume(value), saved(value) {
        if (capture && value <= 0.f) volume = 1.f;
    }
    ~EffectsCreationGuard() { volume = saved; }
};
}

class $modify(SeparateSongEffectsPlayback, FMODAudioEngine) {
    int playEffectAdvanced(gd::string path, float speed, float unknown,
                           float volume, float pitch, bool fastFourierTransform,
                           bool reverb, int startMillis, int endMillis,
                           int fadeIn, int fadeOut, bool loopEnabled,
                           int effectID, bool override, bool noPreload,
                           int channelID, int uniqueID, float minInterval,
                           int sfxGroup) {
        const bool capture = Mod::get()->getSettingValue<bool>("enabled") &&
            separate_song::obs_volume::effects() > 0.f;
        EffectsCreationGuard guard(m_sfxVolume, capture);
        return FMODAudioEngine::playEffectAdvanced(
            path, speed, unknown, volume, pitch, fastFourierTransform, reverb,
            startMillis, endMillis, fadeIn, fadeOut, loopEnabled, effectID,
            override, noPreload, channelID, uniqueID, minInterval, sfxGroup);
    }
};
