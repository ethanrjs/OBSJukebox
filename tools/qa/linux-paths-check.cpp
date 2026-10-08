#include "../../obs-plugin/LinuxPaths.hpp"
#include "../../obs-plugin/LinuxAudioClock.hpp"
#include <cassert>
#include <iostream>
#include <unistd.h>

int main() {
    namespace fs=std::filesystem;
    auto root=fs::temp_directory_path()/("obs-jukebox-path-check-"+std::to_string(getpid()));
    auto prefix=root/"Steam Library/steamapps/compatdata/322170/pfx";
    auto game=root/"Steam Library/steamapps/common/Geometry Dash";
    auto song=prefix/"drive_c/users/steamuser/AppData/Local/GeometryDash/geode/mods/fleym.nongd/nongs/Song Name.mp3";
    fs::create_directories(song.parent_path());std::ofstream(song)<<"song";
    fs::create_directories(game/"Resources");std::ofstream(game/"Resources/StayInsideMe.mp3")<<"game";
    fs::create_directories(prefix/"dosdevices");
    fs::create_directory_symlink(prefix/"drive_c",prefix/"dosdevices/c:");
    fs::create_directory_symlink(root,prefix/"dosdevices/z:");
    fs::create_directories(root/"config/obs-jukebox");
    std::ofstream(root/"config/obs-jukebox/paths")<<prefix.string()<<'\n'<<game.string()<<'\n';
    setenv("XDG_CONFIG_HOME",(root/"config").c_str(),1);
    unsetenv("OBS_JUKEBOX_WINE_PREFIX");unsetenv("OBS_JUKEBOX_GAME_DIRECTORY");
    unsetenv("WINEPREFIX");unsetenv("STEAM_COMPAT_DATA_PATH");
    LinuxPaths paths;
    auto windows="C:\\USERS\\steamuser\\appdata\\local\\geometrydash\\geode\\mods\\fleym.nongd\\nongs\\song name.mp3";
    assert(fs::equivalent(paths.resolve(windows),song));
    assert(fs::equivalent(paths.resolve(std::string("\\\\?\\")+windows),song));
    assert(fs::equivalent(paths.resolve("Resources\\stayinsideme.mp3"),game/"Resources/StayInsideMe.mp3"));
    assert(fs::equivalent(paths.resolve("Z:/Steam Library/steamapps/common/Geometry Dash/Resources/StayInsideMe.mp3"),game/"Resources/StayInsideMe.mp3"));
    assert(paths.resolve(song.string())==song.string());
    assert(paths.resolve("C:/missing.mp3")=="C:/missing.mp3");
    assert(paths.resolve("C:/../outside.mp3")=="C:/../outside.mp3");
    fs::remove(prefix/"dosdevices/c:");
    assert(fs::equivalent(paths.resolve(windows),song));
    setenv("OBS_JUKEBOX_WINE_PREFIX",(root/"wrong-prefix").c_str(),1);
    assert(LinuxPaths().resolve(windows).starts_with("C:/"));
    unsetenv("OBS_JUKEBOX_WINE_PREFIX");
    fs::remove(root/"config/obs-jukebox/paths");
    setenv("STEAM_COMPAT_DATA_PATH",prefix.parent_path().c_str(),1);
    assert(fs::equivalent(LinuxPaths().resolve(windows),song));
    unsetenv("STEAM_COMPAT_DATA_PATH");setenv("WINEPREFIX",prefix.c_str(),1);
    assert(fs::equivalent(LinuxPaths().resolve(windows),song));
    LinuxAudioClock clock;
    uint64_t ts=5000000000,now=9000000000;
    assert(clock.translate(ts,now) && ts==now);
    ts=5010000000;now=9012000000;
    assert(!clock.translate(ts,now) && ts==9010000000);
    ts=10000000;now=9020000000;
    assert(clock.translate(ts,now) && ts==now);
    ts=20000000;now=9030000000;
    assert(!clock.translate(ts,now) && ts==now);
    ts=5000000000;now=12000000000;
    assert(clock.translate(ts,now) && ts==now);
    ts=5010000000;now=12013000000;
    assert(!clock.translate(ts,now) && ts==12010000000);
    fs::remove_all(root);
    std::cout<<"11 path checks and 6 clock checks passed\n";
}
