#!/bin/zsh
set -eu
root=/Users/home/projects/obsjukebox-validation-6zJJ9T
deps=/Users/home/Documents/Codex/2026-10-07/can-you-make-this-mod-in
export GEODE_SDK="$deps/tools/geode-sdk"
export PATH="$deps/tools/python/bin:$PATH"
args=()
for name in arc asp2 fmt json nontype_functional result tuliphook; do
 upper=${(U)name}
 args+=("-DFETCHCONTENT_SOURCE_DIR_${upper}=$root/deps/$name-src")
done
"$deps/tools/cmake-stable/cmake/data/bin/cmake" -S "$root/tools/qa/trigger-smoke" -B "$root/build-trigger-smoke" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DGEODE_BINDINGS_REPO_PATH="$deps/tools/bindings" -DGEODE_CLI="$deps/tools/geode-cli/geode" -DGEODE_DISABLE_PRECOMPILED_HEADERS=ON "${args[@]}"
"$deps/tools/cmake-stable/cmake/data/bin/cmake" --build "$root/build-trigger-smoke" --parallel 4