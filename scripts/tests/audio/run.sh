#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
# --bench builds bench.cpp instead of the tests.
program=playback-tests
if [[ "${1:-}" == --bench ]]; then program=bench; shift; fi
python3 scripts/tests/audio/fixtures.py
flags=(-DOBS_JUKEBOX_QA -std=c++20 -O2 -pthread -Iscripts/tests/audio/stubs "scripts/tests/audio/$program.cpp" obs-plugin/Decoder.cpp obs-plugin/PitchSimd.cpp)
if [[ "$(uname -s)" == Linux ]]; then flags+=(-ldl); fi
"${CXX:-c++}" "${flags[@]}" -o "artifacts/audio-tests/$program"
# Other arguments go to the program: --skip-timing leaves out the tests' real-time checks.
if [[ "${1:-}" != --build-only ]]; then "artifacts/audio-tests/$program" "$@"; fi
