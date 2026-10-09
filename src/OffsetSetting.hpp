#pragma once
#include <Geode/loader/SettingV3.hpp>

namespace separate_song::offset_setting {
class OffsetSetting final : public geode::SettingBaseValueV3<double> {
public:
    static geode::Result<std::shared_ptr<geode::SettingV3>> parse(
        std::string key, std::string modID, matjson::Value const& json);
    geode::Result<> isValid(double value) const override;
    geode::SettingNodeV3* createNode(float width) override;
};
void initialize();
void selectLevel(std::string name);
double value();
void setValue(double value);
}
