#!/bin/zsh
set -euo pipefail
song_root="${0:A:h:h}"
cd "$song_root"
: ${OBS_SDK:="$song_root/tools/obs-sdk"}
: ${SIMDE_INCLUDE:="$song_root/tools/simde"}
: ${OBS_APP:="/Applications/OBS.app"}
: ${MAC_ARCH:=$(uname -m)}
mkdir -p dist/separate-song.plugin/Contents/MacOS
clang++ -std=c++20 -O2 -bundle -arch "$MAC_ARCH" -mmacosx-version-min=13.0 \
  -I"$OBS_SDK/libobs" -I"$SIMDE_INCLUDE" -Iobs-plugin obs-plugin/separate-song.cpp \
  -F"$OBS_APP/Contents/Frameworks" -framework libobs -framework AudioToolbox -framework CoreFoundation \
  -Wl,-rpath,@executable_path/../Frameworks \
  -o dist/separate-song.plugin/Contents/MacOS/separate-song
cp obs-plugin/Info.plist dist/separate-song.plugin/Contents/Info.plist
codesign --force --sign - dist/separate-song.plugin
