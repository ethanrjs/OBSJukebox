#pragma once
#include "Bridge.hpp"
#include <Geode/Geode.hpp>
namespace separate_song::jukebox_link {
void rightClick(cocos2d::CCPoint point);
struct Readiness { bool ready = true; std::string error; };
void initialize();
void refreshUI();
void fill(Snapshot& state, GJGameLevel* level);
Readiness prepare(GJGameLevel* level, bool retry = false);
}
