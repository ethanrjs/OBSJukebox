#!/usr/bin/env bash
set -euo pipefail
song_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
song_output="$song_root/artifacts/linux"
song_mod="$song_root/artifacts/windows/local.separate_song.geode"
while (($#)); do
    case "$1" in
        --mod) song_mod="${2:?Missing mod path}"; shift 2 ;;
        --output) song_output="${2:?Missing output directory}"; shift 2 ;;
        *) printf 'Usage: %s [--mod Windows.geode] [--output directory]\n' "$0" >&2; exit 1 ;;
    esac
done
[[ "$(uname -s)" == Linux ]] || { printf 'Run this build on Linux.\n' >&2; exit 1; }
[[ "$(uname -m)" == x86_64 ]] || { printf 'The Proton package requires x86_64 Linux.\n' >&2; exit 1; }
[[ -f "$song_mod" ]] || { printf 'Build the Windows Geode mod first, then pass its path with --mod.\n' >&2; exit 1; }
for song_tool in cmake pkg-config c++; do
    command -v "$song_tool" >/dev/null || { printf 'Missing build tool: %s\n' "$song_tool" >&2; exit 1; }
done
pkg-config --exists libobs || { printf 'Install the OBS development package (libobs-dev on Ubuntu).\n' >&2; exit 1; }
mkdir -p "$song_output"
song_output="$(cd -- "$song_output" && pwd)"
cmake -S "$song_root/obs-plugin" -B "$song_output/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$song_output/build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
song_package="$song_output/OBS-Jukebox-Linux"
mkdir -p "$song_package/payload"
install -m 755 "$song_output/build/separate-song.so" "$song_package/payload/separate-song.so"
install -m 644 "$song_mod" "$song_package/payload/local.separate_song.geode"
install -m 755 "$song_root/scripts/install-linux.sh" "$song_package/Install.sh"
install -m 755 "$song_root/installer/linux/OBS-Jukebox-Setup" "$song_package/OBS-Jukebox-Setup"
install -m 644 "$song_root/installer/linux/installer.py" "$song_package/installer.py"
install -m 644 "$song_root/installer/windows/logo.png" "$song_package/logo.png"
install -m 644 "$song_root/scripts/check-geode-version.pl" "$song_package/check-geode-version.pl"
install -m 644 "$song_root/LICENSE" "$song_package/LICENSE"
tar -C "$song_output" -czf "$song_output/OBS-Jukebox-Linux-x86_64.tar.gz" OBS-Jukebox-Linux
printf 'Package: %s\n' "$song_output/OBS-Jukebox-Linux-x86_64.tar.gz"
