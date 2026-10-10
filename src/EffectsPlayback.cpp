#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include "ObsVolume.hpp"

using namespace geode::prelude;

// GD returns without starting an effect while its SFX volume is 0, so with the game's effects muted
// none would reach OBS. While OBS takes effects, the volume reads 1 as GD starts one; the effects
// tap sits before the effects group's fader (AudioTap.cpp).
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
        const float saved = m_sfxVolume;
        if (capture && saved <= 0.f) m_sfxVolume = 1.f;
        auto id = FMODAudioEngine::playEffectAdvanced(
            path, speed, unknown, volume, pitch, fastFourierTransform, reverb,
            startMillis, endMillis, fadeIn, fadeOut, loopEnabled, effectID,
            override, noPreload, channelID, uniqueID, minInterval, sfxGroup);
        m_sfxVolume = saved;
        return id;
    }
};
