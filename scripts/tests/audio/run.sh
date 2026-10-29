#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
# --bench builds bench.cpp instead of the tests.
program=playback-tests
if [[ "${1:-}" == --bench ]]; then program=bench; shift; fi
mkdir -p artifacts/audio-tests
command -v ffmpeg >/dev/null || { echo "ffmpeg is required for the MP3 regression fixture" >&2; exit 1; }
ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'sine=frequency=440:duration=240' -ac 2 -ar 48000 -b:a 64k artifacts/audio-tests/long.mp3
ffmpeg -hide_banner -loglevel error -y -i artifacts/audio-tests/long.mp3 -t 4 -c copy artifacts/audio-tests/short.mp3
ffmpeg -hide_banner -loglevel error -y -i artifacts/audio-tests/long.mp3 -c copy -write_xing 0 artifacts/audio-tests/long-noxing.mp3
ffmpeg -hide_banner -loglevel error -y -i artifacts/audio-tests/long.mp3 -ar 44100 -b:a 64k -write_xing 0 artifacts/audio-tests/long-44k-noxing.mp3
flags=(-DOBS_JUKEBOX_QA -std=c++20 -O2 -pthread -Iscripts/tests/audio/stubs "scripts/tests/audio/$program.cpp" obs-plugin/Decoder.cpp)
if [[ "$(uname -s)" == Linux ]]; then flags+=(-ldl); fi
"${CXX:-c++}" "${flags[@]}" -o "artifacts/audio-tests/$program"
# Other arguments go to the program: --skip-timing leaves out the tests' real-time checks.
if [[ "${1:-}" != --build-only ]]; then "artifacts/audio-tests/$program" "$@"; fi
