#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
mkdir -p artifacts/audio-tests
flags=(-std=c++20 -O2 -pthread -Iscripts/tests/audio/stubs scripts/tests/audio/playback-tests.cpp)
if [[ "$(uname -s)" == Linux ]]; then flags+=(-ldl); fi
"${CXX:-c++}" "${flags[@]}" -o artifacts/audio-tests/playback-tests
if [[ "${1:-}" != --build-only ]]; then artifacts/audio-tests/playback-tests; fi
