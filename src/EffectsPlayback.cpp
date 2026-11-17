#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include "AudioTap.hpp"
#include "ObsVolume.hpp"

using namespace geode::prelude;

// GD returns without starting an effect while its SFX volume is 0, so muted effects would never
// reach OBS. The volume reads 1 as GD starts one; the tap sits before the group's fader.
class $modify(SeparateSongEffectsPlayback, FMODAudioEngine) {
    int playEffectAdvanced(
        gd::string path, float speed, float unknown, float volume, float pitch,
        bool fastFourierTransform, bool reverb, int startMillis, int endMillis, int fadeIn,
        int fadeOut, bool loopEnabled, int effectID, bool override, bool noPreload, int channelID,
        int uniqueID, float minInterval, int sfxGroup
    ) {
        bool const capture =
            separate_song::audio_tap::enabled() && separate_song::obs_volume::effects() > 0.f;
        float const saved = m_sfxVolume;
        if (capture && saved <= 0.f) m_sfxVolume = 1.f;
        auto id = FMODAudioEngine::playEffectAdvanced(
            path,
            speed,
            unknown,
            volume,
            pitch,
            fastFourierTransform,
            reverb,
            startMillis,
            endMillis,
            fadeIn,
            fadeOut,
            loopEnabled,
            effectID,
            override,
            noPreload,
            channelID,
            uniqueID,
            minInterval,
            sfxGroup
        );
        m_sfxVolume = saved;
        return id;
    }
};
