#!/bin/zsh
set -euo pipefail
song_root="${0:A:h:h}"
cd "$song_root"
: ${DEPENDENCY_ROOT:="$song_root/tools/pinned"}
python3 scripts/fetch-dependencies.py mac --destination "$DEPENDENCY_ROOT"
: ${GEODE_SDK:="$DEPENDENCY_ROOT/geode-sdk"}
: ${GEODE_BINDINGS:="$DEPENDENCY_ROOT/bindings"}
: ${CMAKE:=cmake}
: ${GEODE_CLI:="$DEPENDENCY_ROOT/geode-cli/geode"}
: ${MAC_ARCH:=$(uname -m)}
mkdir -p "$GEODE_SDK/bin/5.10.1"
cp "$DEPENDENCY_ROOT/geode-macos-5.10.1/Geode.dylib" "$GEODE_SDK/bin/5.10.1/Geode.dylib"
export GEODE_SDK
export OBS_SDK="${OBS_SDK:-$DEPENDENCY_ROOT/obs-sdk}"
export SIMDE_INCLUDE="${SIMDE_INCLUDE:-$DEPENDENCY_ROOT/simde}"
"$CMAKE" -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$MAC_ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DGEODE_BINDINGS_REPO_PATH="$GEODE_BINDINGS" -DGEODE_CLI="$GEODE_CLI" -DGEODE_DISABLE_PRECOMPILED_HEADERS=ON -DOBS_JUKEBOX_QA=OFF "$@"
"$CMAKE" --build build-mac --parallel 4
zsh scripts/build-obs-plugin.sh
mkdir -p artifacts/macos
cp build-mac/local.separate_song.geode artifacts/macos/local.separate_song.geode
