#!/usr/bin/env bash
set -euo pipefail
song_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
song_game=''
song_prefix=''
song_flatpak=0
song_plugin="$song_root/payload/separate-song.so"
song_mod="$song_root/payload/local.separate_song.geode"
while (($#)); do
    case "$1" in
        --game-dir) song_game="${2:?Missing game directory}"; shift 2 ;;
        --wine-prefix) song_prefix="${2:?Missing Wine prefix}"; shift 2 ;;
        --plugin) song_plugin="${2:?Missing plugin path}"; shift 2 ;;
        --mod) song_mod="${2:?Missing mod path}"; shift 2 ;;
        --flatpak) song_flatpak=1; shift ;;
        *) printf 'Usage: %s [--game-dir directory] [--wine-prefix prefix] [--flatpak] [--plugin file.so] [--mod file.geode]\n' "$0" >&2; exit 1 ;;
    esac
done
fail() { printf '%s\n' "$1" >&2; exit 1; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || fail 'This package requires x86_64 Linux with Geometry Dash running through Proton.'
((EUID != 0)) || fail 'Run this installer as your normal desktop user, without sudo.'
[[ -f "$song_plugin" && -f "$song_mod" ]] || fail 'The plugin or Windows Geode mod is missing from the package.'
if pgrep -x obs >/dev/null || pgrep -fi '(^|[/ ])GeometryDash\.exe([ ]|$)' >/dev/null; then
    fail 'Close OBS and Geometry Dash, then run the installer again.'
fi
if [[ -z "$song_game" ]]; then
    for song_steam in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        if [[ -f "$song_steam/steamapps/common/Geometry Dash/GeometryDash.exe" ]]; then
            song_game="$song_steam/steamapps/common/Geometry Dash"
            break
        fi
    done
fi
[[ -n "$song_game" && -f "$song_game/GeometryDash.exe" ]] || fail 'Geometry Dash was not found. Pass its Steam installation directory with --game-dir.'
song_game="$(cd -- "$song_game" && pwd -P)"
[[ -f "$song_game/Geode.dll" && -d "$song_game/geode" ]] || fail 'Install Geode for Windows into Geometry Dash first, then run the game once through Proton.'
if [[ -z "$song_prefix" ]]; then
    song_prefix="$(dirname -- "$(dirname -- "$song_game")")/compatdata/322170/pfx"
fi
[[ -d "$song_prefix/drive_c" ]] || fail 'The Proton prefix was not found. Run Geometry Dash once or pass its Wine prefix with --wine-prefix.'
song_prefix="$(cd -- "$song_prefix" && pwd -P)"
[[ "$song_game" != *$'\n'* && "$song_prefix" != *$'\n'* ]] || fail 'Installation paths cannot contain newlines.'
song_config="${XDG_CONFIG_HOME:-$HOME/.config}"
if ((song_flatpak)); then
    command -v flatpak >/dev/null || fail 'Flatpak is not installed.'
    flatpak info com.obsproject.Studio >/dev/null 2>&1 || fail 'The OBS Flatpak is not installed.'
    song_config="$HOME/.var/app/com.obsproject.Studio/config"
fi
song_destination="$song_config/obs-studio/plugins/separate-song/bin/64bit"
song_backup="${XDG_DATA_HOME:-$HOME/.local/share}/obs-jukebox/backups/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$song_backup" "$song_destination" "$song_game/geode/mods" "$song_config/obs-jukebox"
for song_existing in "$song_destination/separate-song.so" "$song_game/geode/mods/local.separate_song.geode" "$song_config/obs-jukebox/paths"; do
    if [[ -f "$song_existing" ]]; then cp -p -- "$song_existing" "$song_backup/$(basename -- "$song_existing")"; fi
done
install -m 755 "$song_plugin" "$song_destination/separate-song.so"
install -m 644 "$song_mod" "$song_game/geode/mods/local.separate_song.geode"
printf '%s\n%s\n' "$song_prefix" "$song_game" > "$song_config/obs-jukebox/paths"
if ((song_flatpak)); then
    flatpak override --user --filesystem="$song_prefix:ro" --filesystem="$song_game:ro" com.obsproject.Studio
fi
printf 'Installed OBS Jukebox. Backups: %s\nOpen GD and install Jukebox through Geode if needed.\nIn OBS, add the GD Sounds source once. Keep monitoring off and exclude GD audio from your other recording sources.\n' "$song_backup"
