#include "AudioTap.hpp"
#include "Bridge.hpp"
#include "JukeboxLink.hpp"
#include <Geode/loader/GameEvent.hpp>

// Geode dispatches Exiting before CCDirector::purgeDirector on Windows and
// through the native application termination path on macOS. This is outside
// DLL loader-lock teardown and before application audio objects are destroyed.
$on_game(Exiting) {
    separate_song::audio_tap::shutdown();
    separate_song::bridge().shutdown();
    separate_song::jukebox_link::shutdown();
}
