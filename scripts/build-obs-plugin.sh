#!/bin/zsh
set -euo pipefail
song_root="${0:A:h:h}"
cd "$song_root"
: ${OBS_SDK:="$song_root/tools/obs-sdk"}
: ${SIMDE_INCLUDE:="$song_root/tools/simde"}
: ${OBS_APP:="/Applications/OBS.app"}
: ${MAC_ARCH:=$(uname -m)}
song_version=$(/usr/bin/plutil -extract version raw -o - mod.json)
song_gd=$(/usr/bin/plutil -extract gd.win raw -o - mod.json)
rm -rf -- dist/separate-song.plugin
mkdir -p dist/separate-song.plugin/Contents/MacOS
launcher=()
command -v ccache > /dev/null && launcher=(ccache)
"${launcher[@]}" clang++ -std=c++20 -O2 -bundle -arch "$MAC_ARCH" -mmacosx-version-min=13.0 \
    -DOBS_JUKEBOX_VERSION="\"$song_version\"" -DOBS_JUKEBOX_GD="\"$song_gd\"" -fobjc-arc \
    -I"$OBS_SDK/libobs" -I"$SIMDE_INCLUDE" -Iobs-plugin obs-plugin/separate-song.cpp obs-plugin/Decoder.cpp obs-plugin/PitchSimd.cpp \
    obs-plugin/UpdateNotice.cpp obs-plugin/UpdateNoticeMac.mm src/UpdateRules.cpp \
    -F"$OBS_APP/Contents/Frameworks" -framework libobs -framework AudioToolbox -framework CoreFoundation \
    -framework Foundation -framework CoreServices \
    -Wl,-rpath,@executable_path/../Frameworks \
    -o dist/separate-song.plugin/Contents/MacOS/separate-song
cp obs-plugin/Info.plist dist/separate-song.plugin/Contents/Info.plist
/usr/bin/plutil -replace CFBundleShortVersionString -string "${song_version#v}" dist/separate-song.plugin/Contents/Info.plist
/usr/bin/plutil -replace CFBundleVersion -string "${song_version#v}" dist/separate-song.plugin/Contents/Info.plist
codesign --force --sign - dist/separate-song.plugin
