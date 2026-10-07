#include "OffsetSetting.hpp"
#include <Geode/Geode.hpp>
#include <Geode/ui/TextInput.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;
namespace separate_song::offset_setting {
namespace {
class OffsetNode final : public SettingValueNodeV3<OffsetSetting> {
    TextInput* m_input = nullptr;
    Slider* m_slider = nullptr;
    CCMenuItemSpriteExtra* m_left = nullptr;
    CCMenuItemSpriteExtra* m_right = nullptr;

    bool init(std::shared_ptr<OffsetSetting> setting, float width) {
        if (!SettingValueNodeV3::init(setting, width)) return false;
        auto menu = getButtonMenu();
        auto left = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        left->setScale(.5f);
        m_left = CCMenuItemSpriteExtra::create(left, this, menu_selector(OffsetNode::onLeft));
        menu->addChildAtPosition(m_left, Anchor::Left, ccp(22, 0));
        auto right = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        right->setScale(.5f);
        right->setFlipX(true);
        m_right = CCMenuItemSpriteExtra::create(right, this, menu_selector(OffsetNode::onRight));
        menu->addChildAtPosition(m_right, Anchor::Right, ccp(-22, 0));

        m_input = TextInput::create(menu->getContentWidth() - 40, "Seconds");
        m_input->setScale(.7f);
        m_input->setCommonFilter(CommonFilter::Float);
        m_input->setCallback([this](std::string const& text) {
            auto parsed = numFromString<double>(text);
            if (parsed && std::isfinite(parsed.unwrap())) setValue(parsed.unwrap(), m_input);
        });
        menu->addChildAtPosition(m_input, Anchor::Center);
        setContentHeight(45);
        menu->updateAnchoredPosition(Anchor::Right, ccp(-10, 7));
        m_slider = Slider::create(this, menu_selector(OffsetNode::onSlider));
        m_slider->setScale(.5f);
        menu->addChildAtPosition(m_slider, Anchor::Center, ccp(0, -20), ccp(0, 0));
        updateState(nullptr);
        return true;
    }
    void updateState(CCNode* invoker) override {
        SettingValueNodeV3::updateState(invoker);
        if (!m_input || !m_slider) return;
        auto enabled = getSetting()->shouldEnable();
        m_input->setEnabled(enabled);
        m_left->setEnabled(enabled);
        m_right->setEnabled(enabled);
        if (invoker != m_input && (invoker || !m_input->getInputNode()->m_selected)) {
            auto displayed = getValue();
            if (std::abs(displayed) < 1e10) displayed = std::round(displayed * 100000.0) / 100000.0;
            m_input->setString(numToString(displayed));
        }
        m_slider->m_touchLogic->m_thumb->setValue(
            static_cast<float>(std::clamp((getValue() + 10.0) / 20.0, 0.0, 1.0)));
        m_slider->updateBar();
        m_slider->m_touchLogic->m_thumb->setEnabled(enabled);
        m_slider->m_sliderBar->setColor(enabled ? ccWHITE : ccGRAY);
        m_slider->m_touchLogic->m_thumb->setColor(enabled ? ccWHITE : ccGRAY);
    }
    void onLeft(CCObject*) { setValue(getValue() - .1, m_left); }
    void onRight(CCObject*) { setValue(getValue() + .1, m_right); }
    void onSlider(CCObject*) {
        auto value = m_slider->m_touchLogic->m_thumb->getValue() * 20.0 - 10.0;
        setValue(std::round(value * 100.0) / 100.0, m_slider);
    }
public:
    static OffsetNode* create(std::shared_ptr<OffsetSetting> setting, float width) {
        auto node = new OffsetNode();
        if (node->init(std::move(setting), width)) { node->autorelease(); return node; }
        delete node;
        return nullptr;
    }
};
std::shared_ptr<OffsetSetting> get() {
    return std::dynamic_pointer_cast<OffsetSetting>(Mod::get()->getSetting("offset"));
}
}

Result<std::shared_ptr<SettingV3>> OffsetSetting::parse(
    std::string key, std::string modID, matjson::Value const& json) {
    auto setting = std::make_shared<OffsetSetting>();
    auto result = setting->parseBaseProperties(std::move(key), std::move(modID), json);
    if (!result) return Err(result.unwrapErr());
    return Ok(std::static_pointer_cast<SettingV3>(setting));
}
Result<> OffsetSetting::isValid(double value) const {
    if (!std::isfinite(value)) return Err("Enter a finite number");
    return Ok();
}
SettingNodeV3* OffsetSetting::createNode(float width) {
    return OffsetNode::create(std::static_pointer_cast<OffsetSetting>(shared_from_this()), width);
}
void initialize() {
    auto result = Mod::get()->registerCustomSettingType("obs-offset", OffsetSetting::parse);
    if (!result) log::error("Could not register OBS offset setting: {}", result.unwrapErr());
}
double value() { auto setting = get(); return setting ? setting->getValue() : 0.0; }
void setValue(double value) { if (auto setting = get()) setting->setValue(value); }
}
