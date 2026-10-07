#!/bin/zsh
set -euo pipefail
song_root="${0:A:h}"
song_app="${1:-$HOME/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app}"
song_obs="${2:-/Applications/OBS.app}"
[[ -d "$song_obs" ]] || song_obs="$HOME/Applications/OBS.app"
song_audit="$HOME/Library/Application Support/Separate Song/Install Logs/$(date +%Y%m%d-%H%M%S)-$(uuidgen)"
mkdir -p "$song_audit/Backups"
exec > >(tee "$song_audit/install.log") 2>&1
print -r -- "Geometry Dash: $song_app" "OBS Studio: $song_obs" "Install log and backups: $song_audit"
song_copy_count=0
copy_file() {
    (( song_copy_count += 1 ))
    if [[ -e "$2" ]]; then cp -p "$2" "$song_audit/Backups/$song_copy_count-$(basename "$2")"; fi
    cp "$1" "$2"
    print -r -- "Installed: $2"
    shasum -a 256 "$2"
}
finish() { print -r -- "$1"; if [[ -t 0 ]]; then read 'song_reply?Press Return to close. '; fi; }
fail() { finish "$1"; exit 1; }
[[ "$(uname -m)" == arm64 ]] || fail 'This build needs an Apple Silicon Mac.'
[[ -f "$song_app/Contents/MacOS/Geometry Dash" ]] || fail "Geometry Dash was not found. Drag its app into Terminal after this script to use a custom Steam library."
[[ -d "$song_obs" ]] || fail 'Install OBS Studio 32.2 for Apple Silicon first: https://obsproject.com/download'
song_version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$song_obs/Contents/Info.plist")
[[ "$song_version" == 32.2.* ]] || fail "This build was tested with OBS 32.2.2. Your OBS version is $song_version."
if /usr/bin/pgrep -x 'Geometry Dash' >/dev/null || /usr/bin/pgrep -x OBS >/dev/null; then
    fail 'Close Geometry Dash and OBS, then run Install again.'
fi
song_frameworks="$song_app/Contents/Frameworks"
[[ -w "$song_frameworks" ]] || fail 'Your account cannot write to this Steam installation. Install GD in a library owned by your account.'
if [[ ! -f "$song_frameworks/Geode.dylib" ]]; then
    [[ -f "$song_frameworks/libfmod.dylib" ]] || fail 'The original GD audio library is missing. Verify the game in Steam first.'
    [[ ! -e "$song_frameworks/restore_fmod.dylib" ]] || fail 'A partial Geode install already exists. Restore or repair it before installing.'
    /bin/cp -p "$song_frameworks/libfmod.dylib" "$song_frameworks/restore_fmod.dylib"
    /bin/cp "$song_root/payload/geode/Geode.dylib" "$song_frameworks/Geode.dylib"
    /bin/cp "$song_root/payload/geode/GeodeBootstrapper.dylib" "$song_frameworks/GeodeBootstrapper.dylib"
    /bin/mkdir -p "$song_app/Contents/geode/resources/geode.loader"
    /usr/bin/ditto "$song_root/payload/geode/resources" "$song_app/Contents/geode/resources/geode.loader"
    /bin/cp "$song_root/payload/geode/libfmod.dylib" "$song_frameworks/libfmod.dylib"
    print 'Installed Geode 5.10.1.'
else
    print 'Keeping your existing Geode installation.'
fi
/bin/mkdir -p "$song_app/Contents/geode/mods" "$HOME/Library/Application Support/obs-studio/plugins" "$HOME/Music/Separate Song"
copy_file "$song_root/payload/fleym.nongd.geode" "$song_app/Contents/geode/mods/fleym.nongd.geode"
copy_file "$song_root/payload/local.separate_song.geode" "$song_app/Contents/geode/mods/local.separate_song.geode"
if [[ -d "$HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin" ]]; then
    /usr/bin/ditto "$HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin" "$song_audit/Backups/separate-song.plugin"
fi
/usr/bin/ditto "$song_root/payload/separate-song.plugin" "$HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin"
print -r -- "Installed: $HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin"
shasum -a 256 "$HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin/Contents/MacOS/separate-song"
/bin/cp "$song_root/Demo Song.mp3" "$HOME/Music/Separate Song/Demo Song.mp3"
finish $'Installed! No administrator password was needed.\n\n1. Open GD and OBS.\n2. In OBS, Sources + > GD Alternate Song (once).\n3. In GD, right-click a Jukebox song for its purple OBS check.
4. Leave monitoring off and exclude GD/system audio from the recording.\n\nThe included song is in Music/Separate Song. Add it in Jukebox if you want to try it. GD keeps its own selected song.'
