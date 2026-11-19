#include "AudioTap.hpp"
#include "Bridge.hpp"
#include "JukeboxLink.hpp"
#include <Geode/loader/GameEvent.hpp>

// Geode dispatches Exiting before application audio objects are destroyed and outside DLL
// loader-lock teardown, so the joins below are safe.
$on_game(Exiting) {
    separate_song::audio_tap::shutdown();
    separate_song::bridge().shutdown();
    separate_song::jukebox_link::shutdown();
}
