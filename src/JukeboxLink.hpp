#pragma once
#include "Bridge.hpp"
#include <Geode/Geode.hpp>
namespace separate_song::jukebox_link {
void rightClick(cocos2d::CCPoint point);
struct Readiness { bool ready = true; std::string error; };
void initialize();
void shutdown();
// Warns once per session, from the main menu, when Jukebox isn't the version this mod is built for
// and OBS follows game audio.
void showVersionNotice();
void refreshUI();
struct MusicSource { std::string path; std::vector<int> extraIDs; };
using LevelSongs = std::vector<int>;
LevelSongs snapshotSongs(GJGameLevel* level);
// Picks the OBS file for a song. Returns true only when it selected an OBS replacement whose file
// differs from the game's own, so the caller knows the audio OBS plays is not the game's audio.
bool fill(Snapshot& state, const LevelSongs* songs, const MusicSource& source);
Readiness prepare(GJGameLevel* level, bool retry = false);
}
