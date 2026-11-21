#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "ObsVolume.hpp"
#include <algorithm>

using namespace geode::prelude;
using namespace separate_song;

// In OBS mode the slider thumbs call this layer directly. Mega Hack 9 hooks PauseLayer's slider
// callbacks and does not pass the call on, so the game volume changed and OBS did not.
struct SliderCallback {
    CCObject* target = nullptr;
    SEL_MenuHandler selector = nullptr;
};

static SliderThumb* thumbOf(Slider* slider) {
    return slider->m_touchLogic ? slider->m_touchLogic->m_thumb : nullptr;
}

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
        Ref<CCSprite> musicIcon;
        Ref<CCSprite> effectsIcon;
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
        fields->musicIcon = typeinfo_cast<CCSprite*>(getChildByIDRecursive("music-icon"));
        fields->effectsIcon = typeinfo_cast<CCSprite*>(getChildByIDRecursive("sfx-icon"));
        if (!fields->music || !fields->effects || fields->music == fields->effects) return;
        if (!thumbOf(fields->music) || !thumbOf(fields->effects)) return;
        auto play = getChildByIDRecursive("play-button");
        if (!play) return;

        // The toggle shares main-layer with the sliders, so GD 2.2091's hide-UI button hides it too.
        auto layer = fields->music->getParent();
        auto menu = CCMenu::create();
        menu->setID("obs-volume-menu"_spr);
        menu->setPosition(ccp(0, 0));
        fields->button =
            ButtonSprite::create("Game", 54, true, "bigFont.fnt", "GJ_button_01.png", 22, .45f);
        auto button = CCMenuItemSpriteExtra::create(
            fields->button, this, menu_selector(ObsJukeboxPauseVolume::onVolumeTarget)
        );
        button->setID("volume-target-toggle"_spr);

        auto playBox = play->boundingBox();
        float playBottom = layer->convertToNodeSpace(
            play->getParent()->convertToWorldSpace(ccp(playBox.getMinX(), playBox.getMinY()))
        ).y;
        auto thumbTopOf = [layer](Slider* slider) {
            auto thumb = thumbOf(slider);
            auto box = thumb->boundingBox();
            return layer->convertToNodeSpace(
                thumb->getParent()->convertToWorldSpace(ccp(box.getMinX(), box.getMaxY()))
            ).y;
        };
        float thumbTop = std::max(thumbTopOf(fields->music), thumbTopOf(fields->effects));
        float margin = 2.f;
        float shift = std::max(
            0.f,
            button->getScaledContentSize().height + 2.f * margin - (playBottom - thumbTop)
        );
        fields->music->setPositionY(fields->music->getPositionY() - shift);
        fields->effects->setPositionY(fields->effects->getPositionY() - shift);
        if (fields->musicIcon)
            fields->musicIcon->setPositionY(fields->musicIcon->getPositionY() - shift);
        if (fields->effectsIcon)
            fields->effectsIcon->setPositionY(fields->effectsIcon->getPositionY() - shift);

        button->setPosition(ccp(
            (fields->music->getPositionX() + fields->effects->getPositionX()) / 2.f,
            (playBottom + thumbTop - shift) / 2.f
        ));
        menu->addChild(button);
        layer->addChild(menu, 10);
    }

    void onVolumeTarget(CCObject*) {
        auto fields = m_fields.self();
        fields->obs = !fields->obs;
        fields->button->setString(fields->obs ? "OBS" : "Game");
        auto color = fields->obs ? ccc3(120, 210, 255) : ccc3(255, 255, 255);
        fields->button->setColor(color);
        if (fields->musicIcon) fields->musicIcon->setColor(color);
        if (fields->effectsIcon) fields->effectsIcon->setColor(color);
        auto audio = FMODAudioEngine::sharedEngine();
        fields->music->setValue(fields->obs ? obs_volume::music() : audio->getBackgroundMusicVolume());
        fields->effects->setValue(fields->obs ? obs_volume::effects() : audio->m_sfxVolume);
        fields->music->updateBar();
        fields->effects->updateBar();
        // Game callbacks are saved on each switch to OBS, so another mod's callback is restored.
        // One set while in OBS is dropped: it may forward to our handler and change the OBS volume.
        if (fields->obs) {
            fields->gameMusic =
                retarget(fields->music, {this, menu_selector(ObsJukeboxPauseVolume::onObsMusic)});
            fields->gameEffects =
                retarget(fields->effects, {this, menu_selector(ObsJukeboxPauseVolume::onObsEffects)});
        }
        else {
            retarget(fields->music, fields->gameMusic);
            retarget(fields->effects, fields->gameEffects);
        }
    }

    void onObsMusic(CCObject*) {
        obs_volume::setMusic(m_fields->music->getValue());
    }

    void onObsEffects(CCObject*) {
        obs_volume::setEffects(m_fields->effects->getValue());
    }
};
