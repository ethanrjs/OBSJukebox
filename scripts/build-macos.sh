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
geode_version=$(/usr/bin/plutil -extract geode raw -o - mod.json)
geode_version="${geode_version#v}"
mkdir -p "$GEODE_SDK/bin/$geode_version"
cp "$DEPENDENCY_ROOT/geode-macos-$geode_version/Geode.dylib" "$GEODE_SDK/bin/$geode_version/Geode.dylib"
export GEODE_SDK
export OBS_SDK="${OBS_SDK:-$DEPENDENCY_ROOT/obs-sdk}"
export SIMDE_INCLUDE="${SIMDE_INCLUDE:-$DEPENDENCY_ROOT/simde}"
# ccache is optional locally; CI installs it, which makes repeated GeodeBindings builds cheap.
launcher=()
command -v ccache >/dev/null && launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
"$CMAKE" -S . -B build-mac -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$MAC_ARCH" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DGEODE_BINDINGS_REPO_PATH="$GEODE_BINDINGS" -DGEODE_CLI="$GEODE_CLI" -DGEODE_DISABLE_PRECOMPILED_HEADERS=ON -DSKIP_BUILDING_CODEGEN=OFF -DOBS_JUKEBOX_QA=OFF "${launcher[@]}" "$@"
# The plugin build reads only the plugin sources, so it runs alongside the mod build. Its output
# is buffered so the two do not interleave.
song_plugin_log=$(mktemp)
zsh scripts/build-obs-plugin.sh > "$song_plugin_log" 2>&1 &
song_plugin_pid=$!
song_status=0
"$CMAKE" --build build-mac --parallel 4 || song_status=$?
wait "$song_plugin_pid" || song_status=$?
print 'OBS plugin build:'
cat "$song_plugin_log"
rm -f "$song_plugin_log"
if (( song_status != 0 )); then
    print -u2 'The mod build or the OBS plugin build failed.'
    exit 1
fi
mkdir -p artifacts/macos
cp build-mac/local.separate_song.geode artifacts/macos/local.separate_song.geode
