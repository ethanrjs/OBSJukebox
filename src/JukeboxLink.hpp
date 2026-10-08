#pragma once
#include "Bridge.hpp"
#include <Geode/Geode.hpp>
namespace separate_song::jukebox_link {
void rightClick(cocos2d::CCPoint point);
struct Readiness { bool ready = true; std::string error; };
void initialize();
void refreshUI();
struct MusicSource { bool channel = false; std::string path; std::vector<int> extraIDs; };
struct LevelSongs { int initialID; std::string declaredIDs; };
LevelSongs snapshotSongs(GJGameLevel* level);
void fill(Snapshot& state, const LevelSongs* songs, const MusicSource& source);
Readiness prepare(GJGameLevel* level, bool retry = false);
}
