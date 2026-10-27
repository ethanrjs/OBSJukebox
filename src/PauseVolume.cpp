#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "ObsVolume.hpp"

using namespace geode::prelude;
using namespace separate_song;

// In OBS mode the slider thumbs call this layer directly instead of PauseLayer's slider callbacks.
// Mega Hack 9 also hooks those callbacks, and its hook doesn't pass the call on to ours, so the
// game volume changed and OBS didn't.
struct SliderCallback {
    CCObject* target = nullptr;
    SEL_MenuHandler selector = nullptr;
};
static SliderThumb* thumbOf(Slider* slider) {
    return slider->m_touchLogic ? slider->m_touchLogic->m_thumb : nullptr;
}
// Points the slider's thumb at `callback` and returns the callback it had.
static SliderCallback retarget(Slider* slider, SliderCallback callback) {
    auto thumb = thumbOf(slider);
    SliderCallback previous{thumb->m_pListener, thumb->m_pfnSelector};
    thumb->setTarget(callback.target, callback.selector);
    return previous;
}

class $modify(ObsJukeboxPauseVolume, PauseLayer) {
    struct Fields {
        bool obs = false;
        // Retained, so the toggle never touches a freed node if another mod removes or replaces
        // these after setup. It then only changes the detached nodes.
        Ref<Slider> music;
        Ref<Slider> effects;
        Ref<CCLabelBMFont> musicLabel;
        Ref<CCLabelBMFont> effectsLabel;
        std::string gameMusicLabel;
        std::string gameEffectsLabel;
        SliderCallback gameMusic;
        SliderCallback gameEffects;
        ButtonSprite* button = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();
        auto fields = m_fields.self();
        // Node IDs (a required dependency) names these. If another mod removes or replaces them,
        // the checks below leave the pause menu alone.
        fields->music = typeinfo_cast<Slider*>(getChildByIDRecursive("music-slider"));
        fields->effects = typeinfo_cast<Slider*>(getChildByIDRecursive("sfx-slider"));
        fields->musicLabel = typeinfo_cast<CCLabelBMFont*>(getChildByIDRecursive("music-label"));
        fields->effectsLabel = typeinfo_cast<CCLabelBMFont*>(getChildByIDRecursive("sfx-label"));
        if (!fields->music || !fields->effects || fields->music == fields->effects) return;
        if (!thumbOf(fields->music) || !thumbOf(fields->effects)) return;
        if (fields->musicLabel) fields->gameMusicLabel = fields->musicLabel->getString();
        if (fields->effectsLabel) fields->gameEffectsLabel = fields->effectsLabel->getString();

        auto musicPosition = convertToNodeSpace(fields->music->getParent()->convertToWorldSpace(fields->music->getPosition()));
        auto effectsPosition = convertToNodeSpace(fields->effects->getParent()->convertToWorldSpace(fields->effects->getPosition()));
        auto menu = CCMenu::create();
        menu->setID("obs-volume-menu"_spr);
        menu->setPosition(ccp(0, 0));
        fields->button = ButtonSprite::create("Game", 54, true, "bigFont.fnt", "GJ_button_01.png", 22, .45f);
        auto button = CCMenuItemSpriteExtra::create(fields->button, this, menu_selector(ObsJukeboxPauseVolume::onVolumeTarget));
        button->setID("volume-target-toggle"_spr);
        auto targetPosition = (musicPosition + effectsPosition) / 2.f + ccp(0, 23);
        if (fields->musicLabel && fields->effectsLabel) {
            auto musicHeading = convertToNodeSpace(fields->musicLabel->getParent()->convertToWorldSpace(fields->musicLabel->getPosition()));
            auto effectsHeading = convertToNodeSpace(fields->effectsLabel->getParent()->convertToWorldSpace(fields->effectsLabel->getPosition()));
            targetPosition.y = (musicHeading.y + effectsHeading.y) / 2.f;
        }
        button->setPosition(targetPosition);
        menu->addChild(button);
        addChild(menu, 10);
    }

    void onVolumeTarget(CCObject*) {
        auto fields = m_fields.self();
        fields->obs = !fields->obs;
        fields->button->setString(fields->obs ? "OBS" : "Game");
        fields->button->setColor(fields->obs ? ccc3(120, 210, 255) : ccc3(255, 255, 255));
        if (fields->musicLabel) fields->musicLabel->setString(fields->obs ? "OBS Music" : fields->gameMusicLabel.c_str());
        if (fields->effectsLabel) fields->effectsLabel->setString(fields->obs ? "OBS SFX" : fields->gameEffectsLabel.c_str());
        auto audio = FMODAudioEngine::sharedEngine();
        fields->music->setValue(fields->obs ? obs_volume::music() : audio->getBackgroundMusicVolume());
        fields->effects->setValue(fields->obs ? obs_volume::effects() : audio->getEffectsVolume());
        fields->music->updateBar();
        fields->effects->updateBar();
        // The Game callbacks are saved on each switch to OBS, so a callback another mod set while in
        // Game mode is the one restored. One set while in OBS mode is overwritten on the way back:
        // it may forward to our OBS handler, and Game mode must never change the OBS volume.
        if (fields->obs) {
            fields->gameMusic = retarget(fields->music, {this, menu_selector(ObsJukeboxPauseVolume::onObsMusic)});
            fields->gameEffects = retarget(fields->effects, {this, menu_selector(ObsJukeboxPauseVolume::onObsEffects)});
        } else {
            retarget(fields->music, fields->gameMusic);
            retarget(fields->effects, fields->gameEffects);
        }
    }

    void onObsMusic(CCObject*) { obs_volume::setMusic(m_fields->music->getValue()); }
    void onObsEffects(CCObject*) { obs_volume::setEffects(m_fields->effects->getValue()); }
};
