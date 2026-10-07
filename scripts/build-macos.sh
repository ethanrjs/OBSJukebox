#!/bin/zsh
set -euo pipefail
song_root="${0:A:h:h}"
cd "$song_root"
: ${GEODE_SDK:="$song_root/tools/geode-sdk"}
: ${GEODE_BINDINGS:="$song_root/tools/bindings"}
: ${CMAKE:=cmake}
: ${GEODE_CLI:=geode}
: ${MAC_ARCH:=$(uname -m)}
export GEODE_SDK
"$CMAKE" -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$MAC_ARCH" -DGEODE_BINDINGS_REPO_PATH="$GEODE_BINDINGS" -DGEODE_CLI="$GEODE_CLI" -DGEODE_DISABLE_PRECOMPILED_HEADERS=ON "$@"
"$CMAKE" --build build-mac --parallel 4
zsh scripts/build-obs-plugin.sh
mkdir -p artifacts/macos
cp build-mac/local.separate_song.geode artifacts/macos/local.separate_song.geode
