#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include "ObsVolume.hpp"

using namespace geode::prelude;
using namespace separate_song;

namespace {
void findVolumeControls(CCNode* node, std::vector<Slider*>& sliders,
                        CCLabelBMFont*& musicLabel, CCLabelBMFont*& effectsLabel) {
    if (auto slider = typeinfo_cast<Slider*>(node)) sliders.push_back(slider);
    if (auto label = typeinfo_cast<CCLabelBMFont*>(node)) {
        std::string text = label->getString();
        if (text == "Music") musicLabel = label;
        else if (text == "SFX") effectsLabel = label;
    }
    for (auto child : CCArrayExt<CCNode*>(node->getChildren()))
        findVolumeControls(child, sliders, musicLabel, effectsLabel);
}
}

class $modify(ObsJukeboxPauseVolume, PauseLayer) {
    struct Fields {
        bool obs = false;
        Slider* music = nullptr;
        Slider* effects = nullptr;
        CCLabelBMFont* musicLabel = nullptr;
        CCLabelBMFont* effectsLabel = nullptr;
        std::string gameMusicLabel;
        std::string gameEffectsLabel;
        ButtonSprite* button = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();
        auto fields = m_fields.self();
        fields->music = typeinfo_cast<Slider*>(getChildByIDRecursive("music-slider"));
        fields->effects = typeinfo_cast<Slider*>(getChildByIDRecursive("sfx-slider"));
        fields->musicLabel = typeinfo_cast<CCLabelBMFont*>(getChildByIDRecursive("music-label"));
        fields->effectsLabel = typeinfo_cast<CCLabelBMFont*>(getChildByIDRecursive("sfx-label"));
        if (!fields->music || !fields->effects || !fields->musicLabel || !fields->effectsLabel) {
            std::vector<Slider*> sliders;
            CCLabelBMFont* musicLabel = nullptr;
            CCLabelBMFont* effectsLabel = nullptr;
            findVolumeControls(this, sliders, musicLabel, effectsLabel);
            if (sliders.size() == 2) {
                std::sort(sliders.begin(), sliders.end(), [](Slider* left, Slider* right) {
                    return left->getParent()->convertToWorldSpace(left->getPosition()).x <
                           right->getParent()->convertToWorldSpace(right->getPosition()).x;
                });
                if (!fields->music) fields->music = sliders[0];
                if (!fields->effects) fields->effects = sliders[1];
                if (!fields->musicLabel) fields->musicLabel = musicLabel;
                if (!fields->effectsLabel) fields->effectsLabel = effectsLabel;
            }
        }
        if (!fields->music || !fields->effects || fields->music == fields->effects) return;
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
    }

    void musicSliderChanged(CCObject* sender) {
        if (m_fields->obs && m_fields->music) obs_volume::setMusic(m_fields->music->getValue());
        else PauseLayer::musicSliderChanged(sender);
    }

    void sfxSliderChanged(CCObject* sender) {
        if (m_fields->obs && m_fields->effects) obs_volume::setEffects(m_fields->effects->getValue());
        else PauseLayer::sfxSliderChanged(sender);
    }
};
