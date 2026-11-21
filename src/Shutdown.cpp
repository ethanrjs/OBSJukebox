#include "AudioTap.hpp"
#include "Bridge.hpp"
#include "JukeboxLink.hpp"
#include <Geode/loader/GameEvent.hpp>

$on_game(Exiting) {
    separate_song::audio_tap::shutdown();
    separate_song::bridge().shutdown();
    separate_song::jukebox_link::shutdown();
}
