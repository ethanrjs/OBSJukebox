#!/bin/zsh
set -euo pipefail
song_root="${0:A:h}"
song_uninstall=0
if [[ "${1:-}" == --uninstall ]]; then song_uninstall=1; shift; fi
song_app="${1:-$HOME/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app}"
song_obs="${2:-/Applications/OBS.app}"
if [[ $# -lt 2 && ! -d "$song_obs" ]]; then song_obs="$HOME/Applications/OBS.app"; fi
song_app="${song_app:A}"
song_obs="${song_obs:A}"
finish() { print -r -- "$1"; if [[ -t 0 ]]; then read 'song_reply?Press Return to close. '; fi; }
fail() { finish "$1"; exit 1; }
[[ "$(uname -s)" == Darwin ]] || fail 'Run this installer on macOS.'
if (( song_uninstall )); then
    if /usr/bin/pgrep -x 'Geometry Dash' >/dev/null || /usr/bin/pgrep -x obs >/dev/null; then fail 'Close Geometry Dash and OBS before undoing installation.'; fi
    /usr/bin/perl "$song_root/install-state.pl" undo-latest "$HOME/Library/Application Support/OBS Jukebox/Install Logs"
    exit
fi
[[ -f "$song_root/install-state.pl" ]] || fail 'The installer is missing its undo helper.'
[[ -f "$song_app/Contents/MacOS/Geometry Dash" ]] || fail 'Geometry Dash was not found. Choose its application in your Steam library.'
[[ -f "$song_obs/Contents/Info.plist" ]] || fail 'OBS Studio was not found. Install OBS Studio first or choose its application.'
song_obs_executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$song_obs/Contents/Info.plist")
[[ -f "$song_obs/Contents/MacOS/$song_obs_executable" ]] || fail 'The selected OBS application is incomplete.'
song_plugin="$song_root/payload/separate-song.plugin/Contents/MacOS/separate-song"
for song_file in "$song_root/payload/local.separate_song.geode" "$song_root/payload/fleym.nongd.geode" "$song_root/payload/separate-song.plugin/Contents/Info.plist" "$song_plugin"; do
    [[ -s "$song_file" ]] || fail "The installer is incomplete: $song_file"
done
song_arch=$(uname -m)
/usr/bin/lipo -verify_arch "$song_arch" "$song_plugin" >/dev/null 2>&1 || fail "This installer does not include a plugin for $song_arch."
/usr/bin/lipo -verify_arch "$song_arch" "$song_obs/Contents/MacOS/$song_obs_executable" >/dev/null 2>&1 || fail 'Install the native OBS Studio build for this Mac before continuing.'
if /usr/bin/pgrep -x 'Geometry Dash' >/dev/null || /usr/bin/pgrep -x "$song_obs_executable" >/dev/null; then
    fail 'Finish any OBS recording, close Geometry Dash and OBS, then click Install again.'
fi
song_frameworks="$song_app/Contents/Frameworks"
[[ -d "$song_frameworks" && -w "$song_frameworks" ]] || fail 'Your account cannot write to this Steam installation. Choose a Steam library owned by your account.'
song_install_geode=0
[[ -f "$song_root/check-geode-version.pl" ]] || fail 'The installer is missing its Geode compatibility check. Download the complete package again.'
if [[ ! -f "$song_frameworks/Geode.dylib" ]]; then
    song_install_geode=1
    [[ -f "$song_frameworks/libfmod.dylib" ]] || fail 'The original GD audio library is missing. Verify the game in Steam first.'
    [[ ! -e "$song_frameworks/restore_fmod.dylib" ]] || fail 'A partial Geode install already exists. Restore or repair it before installing.'
    for song_file in Geode.dylib GeodeBootstrapper.dylib libfmod.dylib; do
        [[ -s "$song_root/payload/geode/$song_file" ]] || fail "The installer is missing Geode's $song_file."
    done
    [[ -d "$song_root/payload/geode/resources" ]] || fail 'The installer is missing Geode resources.'
    song_geode_library="$song_root/payload/geode/Geode.dylib"
else
    song_geode_library="$song_frameworks/Geode.dylib"
fi
song_geode_version=$(/usr/bin/perl "$song_root/check-geode-version.pl" "$song_geode_library") || fail 'Geode compatibility could not be verified. Update or repair Geode with the official installer from https://geode-sdk.org (Geode >=5.10.1 and <6.0.0), then try again. No installation files have been changed.'
song_mods="$song_app/Contents/geode/mods"
song_packages=$(/usr/bin/perl -MJSON::PP - "$song_mods" <<'PERL'
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
        open my $listing, '-|', '/usr/bin/unzip', '-Z1', $path or die "Cannot inspect $path\n";
        @entries = <$listing>;
        close $listing or die "Cannot inspect $path\n";
        chomp @entries;
        die "Invalid or ambiguous mod.json in $path\n" unless (grep { $_ eq 'mod.json' } @entries) == 1;
        open my $metadata, '-|', '/usr/bin/unzip', '-p', $path, 'mod.json' or die "Cannot read $path\n";
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
            die "Installed Jukebox has no macOS support: $path\n" unless grep { $_ eq 'fleym.nongd.dylib' } @entries;
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
song_audit="$HOME/Library/Application Support/OBS Jukebox/Install Logs/$(date +%Y%m%d-%H%M%S)-$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.6f",time')-$(uuidgen)"
mkdir -p "$song_audit/Backups" "$song_audit/Failed"
exec > >(tee "$song_audit/install.log") 2>&1
print -r -- 'OBS Jukebox 1.2.0' "Geometry Dash: $song_app" "OBS Studio: $song_obs" "Install log and backups: $song_audit"
typeset -a song_targets song_previous
song_success=0
rollback() {
    local song_result=$?
    trap - EXIT
    if (( ! song_success )); then
        print 'Installation failed. Restoring changed files.'
        local song_i
        for (( song_i=${#song_targets}; song_i>=1; song_i-- )); do
            if [[ -e "${song_targets[$song_i]}" ]]; then
                /bin/mv "${song_targets[$song_i]}" "$song_audit/Failed/$song_i" || true
            fi
            if [[ "${song_previous[$song_i]}" == 1 ]]; then
                /usr/bin/ditto "$song_audit/Backups/$song_i" "${song_targets[$song_i]}" || print -r -- "Restore manually: $song_audit/Backups/$song_i -> ${song_targets[$song_i]}"
            fi
        done
    fi
    exit "$song_result"
}
trap rollback EXIT
copy_item() {
    local song_source="$1" song_destination="$2" song_index=$(( ${#song_targets} + 1 )) song_existed=0
    [[ ! -L "$song_destination" ]] || fail "Refusing to replace a symbolic link: $song_destination"
    if [[ -e "$song_destination" ]]; then
        /usr/bin/ditto "$song_destination" "$song_audit/Backups/$song_index"
        song_existed=1
    fi
    song_targets+=("$song_destination")
    song_previous+=("$song_existed")
    /bin/mkdir -p "${song_destination:h}"
    # Replace complete bundles rather than merging stale files into new payloads.
    if [[ -e "$song_destination" ]]; then /bin/mv "$song_destination" "$song_audit/Failed/replaced-$song_index"; fi
    /usr/bin/ditto --noextattr --noqtn "$song_source" "$song_destination"
    /usr/bin/perl "$song_root/install-state.pl" record "$song_audit" "$song_destination" "$song_audit/Backups/$song_index"
    print -r -- "Installed: $song_destination"
    if [[ -f "$song_destination" ]]; then /usr/bin/shasum -a 256 "$song_destination"; fi
}
if (( song_install_geode )); then
    copy_item "$song_frameworks/libfmod.dylib" "$song_frameworks/restore_fmod.dylib"
    copy_item "$song_root/payload/geode/Geode.dylib" "$song_frameworks/Geode.dylib"
    copy_item "$song_root/payload/geode/GeodeBootstrapper.dylib" "$song_frameworks/GeodeBootstrapper.dylib"
    copy_item "$song_root/payload/geode/resources" "$song_app/Contents/geode/resources/geode.loader"
    copy_item "$song_root/payload/geode/libfmod.dylib" "$song_frameworks/libfmod.dylib"
    print -r -- "Installed Geode $song_geode_version."
else
    print -r -- "Keeping your compatible Geode $song_geode_version installation."
fi
if [[ "$song_jukebox" == - ]]; then
    copy_item "$song_root/payload/fleym.nongd.geode" "$song_mods/fleym.nongd.geode"
else
    print -r -- "Keeping your compatible Jukebox 3.8.0 installation: $song_jukebox"
fi
copy_item "$song_root/payload/local.separate_song.geode" "$song_mod_destination"
copy_item "$song_root/payload/separate-song.plugin" "$HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin"
/usr/bin/shasum -a 256 "$HOME/Library/Application Support/obs-studio/plugins/separate-song.plugin/Contents/MacOS/separate-song"
touch "$song_audit/complete"
song_success=1
print -r -- 'Undo this installation with Install.command --uninstall.'
finish $'Installed!\n\n1. Open Geometry Dash and OBS.\n2. In OBS, add Sources > GD Sounds once. Keep Audio Monitoring off.\n3. In Jukebox, use the Game and OBS checkboxes to choose each song.\n4. In the pause menu, switch Game / OBS to adjust their volumes.\n\nGD Sounds includes game sound effects. Disable any duplicate GD audio capture in OBS.'
