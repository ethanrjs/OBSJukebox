#!/usr/bin/env bash
set -euo pipefail
song_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
song_game=''
song_prefix=''
song_flatpak=0
song_uninstall=0
song_plugin="$song_root/payload/separate-song.so"
song_mod="$song_root/payload/local.separate_song.geode"
while (($#)); do
    case "$1" in
        --game-dir) song_game="${2:?Missing game directory}"; shift 2 ;;
        --wine-prefix) song_prefix="${2:?Missing Wine prefix}"; shift 2 ;;
        --plugin) song_plugin="${2:?Missing plugin path}"; shift 2 ;;
        --mod) song_mod="${2:?Missing mod path}"; shift 2 ;;
        --uninstall) song_uninstall=1; shift ;;
        --flatpak) song_flatpak=1; shift ;;
        *) printf 'Usage: %s [--uninstall] [--game-dir directory] [--wine-prefix prefix] [--flatpak] [--plugin file.so] [--mod file.geode]\n' "$0" >&2; exit 1 ;;
    esac
done
fail() { printf '%s\n' "$1" >&2; exit 1; }
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || fail 'This package requires x86_64 Linux with Geometry Dash running through Proton.'
((EUID != 0)) || fail 'Run this installer as your normal desktop user, without sudo.'
if pgrep -x obs >/dev/null || pgrep -fi '(^|[/\\ ])GeometryDash\.exe([ ]|$)' >/dev/null; then
    fail 'Close OBS and Geometry Dash, then run the installer again.'
fi
[[ -f "$song_root/install-state.pl" ]] || fail 'The installer is missing its undo helper.'
if ((song_uninstall)); then
    perl "$song_root/install-state.pl" undo-latest "${XDG_DATA_HOME:-$HOME/.local/share}/obs-jukebox/backups"
    exit
fi
[[ -f "$song_plugin" && -f "$song_mod" ]] || fail 'The plugin or Windows Geode mod is missing from the package.'
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
[[ -f "$song_root/check-geode-version.pl" ]] || fail 'The installer is missing its Geode compatibility check. Download the complete package again.'
command -v perl >/dev/null && perl -MJSON::PP -e 1 >/dev/null 2>&1 || fail 'Install Perl (including its core JSON::PP module) with your Linux package manager, then run this installer again.'
song_geode_version=$(perl "$song_root/check-geode-version.pl" "$song_game/Geode.dll") || fail 'Geode compatibility could not be verified. Update or repair Geode for Windows with the official installer from https://geode-sdk.org (Geode >=5.10.1 and <6.0.0), then try again. No installation files have been changed.'
song_mods="$song_game/geode/mods"
song_packages=$(perl -MJSON::PP - "$song_mods" <<'PERL'
use strict;
use warnings;
my ($directory) = @ARGV;
my %installed;
if (-d $directory) {
    opendir my $mods, $directory or die "Cannot read installed mods: $!\n";
    for my $name (sort grep { /\.geode\z/i } readdir $mods) {
        my $path = "$directory/$name";
        my $canonical = lc($name) eq 'fleym.nongd.geode' || lc($name) eq 'local.separate_song.geode';
        my (@entries, $mod);
        my $valid = eval {
        die "Repair the installed package before continuing: $path\n" if !-f $path || $path =~ /[\r\n]/;
        open my $listing, '-|', 'unzip', '-Z1', $path or die "Cannot inspect $path\n";
        @entries = <$listing>;
        close $listing or die "Cannot inspect $path\n";
        chomp @entries;
        die "Invalid or ambiguous mod.json in $path\n" unless (grep { $_ eq 'mod.json' } @entries) == 1;
        open my $metadata, '-|', 'unzip', '-p', $path, 'mod.json' or die "Cannot read $path\n";
        my $json = do { local $/; <$metadata> };
        close $metadata or die "Cannot read $path\n";
        $mod = JSON::PP->new->relaxed->decode($json);
        die "Invalid mod.json in $path\n" unless ref($mod) eq 'HASH' && defined($mod->{id}) && !ref($mod->{id}) && $mod->{id} =~ /\A[a-z0-9_-]+\.[a-z0-9_.-]+\z/i;
        1;
        };
        if (!$valid) { die $@ if $canonical; next; }
        my $id = $mod->{id};
        die "Unexpected mod ID in $path\n" if $canonical && "$id.geode" ne lc($name);
        next unless $id eq 'fleym.nongd' || $id eq 'local.separate_song';
        die "Refusing to replace or duplicate a symbolic link: $path\n" if -l $path;
        die "Multiple installed packages have ID $id. Remove the duplicate before continuing.\n" if exists $installed{$id};
        die "Invalid mod version in $path\n" unless defined($mod->{version}) && !ref($mod->{version}) && $mod->{version} =~ /\Av?\d+\.\d+\.\d+(?:[-+][a-zA-Z0-9.+-]+)?\z/;
        if ($id eq 'fleym.nongd') {
            die "Install Jukebox 3.8.0 from Geode before continuing.\n" unless $mod->{version} =~ /\Av?3\.8\.0\z/;
            die "Installed Jukebox has no Windows support: $path\n" unless grep { $_ eq 'fleym.nongd.dll' } @entries;
        }
        $installed{$id} = $path;
    }
    closedir $mods;
}
print(($installed{'fleym.nongd'} // '-'), "\n", ($installed{'local.separate_song'} // "$directory/local.separate_song.geode"));
PERL
) || fail 'Installed mod compatibility could not be verified. No installation files have been changed.'
song_jukebox=${song_packages%%$'\n'*}
song_mod_destination=${song_packages#*$'\n'}
[[ "$song_jukebox" != - ]] || fail 'Install Jukebox 3.8.0 through Geode in Geometry Dash before installing OBS Jukebox.'
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
song_targets=("$song_destination/separate-song.so" "$song_mod_destination" "$song_config/obs-jukebox/paths")
song_sources=("$song_plugin" "$song_mod")
song_modes=(755 644 644)
song_staged=()
song_originals=()
song_changed=()
song_committed=0
if ((song_flatpak)); then
    song_targets+=("${XDG_DATA_HOME:-$HOME/.local/share}/flatpak/overrides/com.obsproject.Studio")
fi
song_cleanup() {
    song_status=$?
    trap - EXIT HUP INT TERM
    set +e
    if ((song_committed == 0)); then
        for ((song_i=${#song_targets[@]}-1; song_i>=0; song_i--)); do
            if [[ "${song_changed[song_i]:-0}" == 1 ]]; then
                if [[ -n "${song_originals[song_i]:-}" ]]; then
                    if ! mv -f -- "${song_originals[song_i]}" "${song_targets[song_i]}"; then
                        printf 'Could not restore %s. Backup retained at %s.\n' "${song_targets[song_i]}" "${song_originals[song_i]}" >&2
                        song_originals[song_i]=''
                        song_status=1
                    fi
                elif ! rm -f -- "${song_targets[song_i]}"; then
                    printf 'Could not remove incomplete installation file %s.\n' "${song_targets[song_i]}" >&2
                    song_status=1
                fi
            fi
        done
    fi
    for song_temp in "${song_staged[@]}" "${song_originals[@]}"; do
        [[ -z "$song_temp" ]] || rm -f -- "$song_temp"
    done
    exit "$song_status"
}
trap song_cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$song_backup"
for song_i in "${!song_targets[@]}"; do
    song_target="${song_targets[song_i]}"
    [[ ! -L "$song_target" && ( ! -e "$song_target" || -f "$song_target" ) ]] || fail "Installation target is not a regular file: $song_target"
    song_parent="$(dirname -- "$song_target")"
    mkdir -p "$song_parent"
    song_originals[song_i]=''
    song_changed[song_i]=0
    if [[ -f "$song_target" ]]; then
        cp -p -- "$song_target" "$song_backup/$(basename -- "$song_target")"
        song_originals[song_i]="$(mktemp "$song_parent/.obs-jukebox-original.XXXXXX")"
        cp -p -- "$song_target" "${song_originals[song_i]}"
    fi
    if ((song_i < 3)); then
        song_staged[song_i]="$(mktemp "$song_parent/.obs-jukebox-new.XXXXXX")"
        if ((song_i < 2)); then
            install -m "${song_modes[song_i]}" "${song_sources[song_i]}" "${song_staged[song_i]}"
        else
            printf '%s\n%s\n' "$song_prefix" "$song_game" > "${song_staged[song_i]}"
            chmod 644 "${song_staged[song_i]}"
        fi
    else
        song_temp="$(mktemp "$song_parent/.obs-jukebox-check.XXXXXX")"
        rm -f -- "$song_temp"
    fi
done
for song_i in 0 1 2; do
    song_changed[song_i]=1
    mv -f -- "${song_staged[song_i]}" "${song_targets[song_i]}"
done
if ((song_flatpak)); then
    song_changed[3]=1
    flatpak override --user --filesystem="$song_prefix:ro" --filesystem="$song_game:ro" com.obsproject.Studio
fi
for song_i in "${!song_targets[@]}"; do
    perl "$song_root/install-state.pl" record "$song_backup" "${song_targets[song_i]}" "$song_backup/$(basename -- "${song_targets[song_i]}")"
done
touch "$song_backup/complete"
song_committed=1
printf 'Installed OBS Jukebox with Geode %s. Backups: %s\nUndo this installation with Install.sh --uninstall.\nIn OBS, add the GD Sounds source once. Keep monitoring off and exclude GD audio from your other recording sources.\n' "$song_geode_version" "$song_backup"
