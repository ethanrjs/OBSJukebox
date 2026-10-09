#!/bin/zsh
set -eu
root=${OBS_JUKEBOX_ROOT:-${0:A:h:h:h:h}}
export GEODE_SDK=${GEODE_SDK:-$HOME/geode}
cmake=${CMAKE:-cmake}
args=()
for name in arc asp2 fmt json nontype_functional result tuliphook; do
 if [[ -d "$root/deps/$name-src" ]]; then
  upper=${(U)name}
  args+=("-DFETCHCONTENT_SOURCE_DIR_${upper}=$root/deps/$name-src")
 fi
done
[[ -z ${GEODE_BINDINGS_REPO_PATH:-} ]] || args+=("-DGEODE_BINDINGS_REPO_PATH=$GEODE_BINDINGS_REPO_PATH")
[[ -z ${GEODE_CLI:-} ]] || args+=("-DGEODE_CLI=$GEODE_CLI")
"$cmake" -S "$root/tools/qa/trigger-smoke" -B "$root/build-trigger-smoke" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=${OBS_JUKEBOX_ARCH:-arm64} -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DGEODE_DISABLE_PRECOMPILED_HEADERS=ON "${args[@]}"
"$cmake" --build "$root/build-trigger-smoke" --parallel 4
