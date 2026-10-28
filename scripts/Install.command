#!/bin/zsh
set -euo pipefail
zmodload zsh/system
song_root="${0:A:h}"
song_uninstall=0 song_update=0 song_no_downgrade=0 song_result_file='' song_reopen_flag=''
# Exit codes the update wrapper reads: 0 installed, 1 failed, 2 busy or apps open, 3 rollback
# incomplete, 4 a newer mod is installed. 2 and 4 mean nothing was changed.
song_exit=1 song_success=0 song_repair=0
typeset -a song_targets song_previous
finish() { print -r -- "$1"; if [[ -t 0 ]]; then read 'song_reply?Press Return to close. '; fi; }
fail() { finish "$1"; exit 1; }
# Stops before changing anything, with a code the update wrapper acts on.
stop() { song_exit=$1; finish "$2"; exit "$1"; }
# An fcntl lock the shell holds until it exits; the kernel releases it even after a crash.
take_lock() {
    local song_dir="$HOME/Library/Application Support/OBS Jukebox"
    /bin/mkdir -p "$song_dir"
    : >> "$song_dir/$1"
    zsystem flock -t 0 "$song_dir/$1" 2>/dev/null
}
# True when $1 is a release version above $2 ("v" prefixes allowed). Anything else isn't newer.
version_newer() {
    /usr/bin/perl -e 'my @v = map { /\Av?(\d+)\.(\d+)\.(\d+)\z/ ? [$1, $2, $3] : undef } @ARGV;
        exit 1 unless $v[0] && $v[1];
        for my $i (0 .. 2) { exit 0 if $v[0][$i] > $v[1][$i]; exit 1 if $v[0][$i] < $v[1][$i] }
        exit 1' -- "$1" "$2"
}
# Restores this run's changes if it didn't finish, then exits with one of the codes above. Installed
# first, so no failure, however early, passes its raw status on.
finalize() {
    trap - EXIT
    if (( ! song_success && ${#song_targets} )); then
        print 'Installation failed. Restoring changed files.'
        local song_i
        for (( song_i=${#song_targets}; song_i>=1; song_i-- )); do
            if [[ -e "${song_targets[$song_i]}" ]]; then
                /bin/mv "${song_targets[$song_i]}" "$song_audit/Failed/$song_i" || song_repair=1
            fi
            if [[ "${song_previous[$song_i]}" == 1 ]]; then
                /usr/bin/ditto "$song_audit/Backups/$song_i" "${song_targets[$song_i]}" || { song_repair=1; print -r -- "Restore manually: $song_audit/Backups/$song_i -> ${song_targets[$song_i]}"; }
            fi
        done
    fi
    if (( song_success )); then exit 0
    elif (( song_repair )); then exit 3
    elif (( song_exit == 2 || song_exit == 4 )); then exit "$song_exit"
    else exit 1; fi
}
trap finalize EXIT
while [[ "${1:-}" == --* ]]; do
    case "$1" in
        --uninstall) song_uninstall=1 ;;
        --update) song_update=1 ;;
        --no-downgrade) song_no_downgrade=1 ;;
        --result) song_result_file="${2:?--result needs a path}"; shift ;;
        --reopen-flag) song_reopen_flag="${2:?--reopen-flag needs a path}"; shift ;;
        *) fail "Unknown option: $1" ;;
    esac
    shift
done
song_app="${1:-$HOME/Library/Application Support/Steam/steamapps/common/Geometry Dash/Geometry Dash.app}"
song_obs="${2:-/Applications/OBS.app}"
if [[ $# -lt 2 && ! -d "$song_obs" ]]; then song_obs="$HOME/Applications/OBS.app"; fi
song_app="${song_app:A}"
song_obs="${song_obs:A}"
[[ "$(/usr/bin/uname -s)" == Darwin ]] || fail 'Run this installer on macOS.'
if (( song_uninstall )); then
    take_lock install.lock || stop 2 'Another OBS Jukebox setup is running. Let it finish, then try again.'
    if /usr/bin/pgrep -x 'Geometry Dash' >/dev/null || /usr/bin/pgrep -x obs >/dev/null; then stop 2 'Close Geometry Dash and OBS before undoing installation.'; fi
    /usr/bin/perl "$song_root/install-state.pl" undo-latest "$HOME/Library/Application Support/OBS Jukebox/Install Logs"
    song_success=1
    exit
fi
take_reopen_flag() { [[ -n "$song_reopen_flag" && -e "$song_reopen_flag" ]] && /bin/rm -f "$song_reopen_flag"; }
if (( song_update )); then
    [[ -n "$song_result_file" ]] || fail 'Update mode needs --result.'
    song_payload_version=$(/usr/bin/unzip -p "$song_root/payload/local.separate_song.geode" mod.json | /usr/bin/perl -MJSON::PP -e 'my $v = JSON::PP->new->relaxed->decode(join "", <STDIN>)->{version}; $v =~ s/\Av//; print $v')
    write_result() {
        /usr/bin/env SONG_STATUS="$1" SONG_MESSAGE="${2:-}" SONG_REPAIR="${3:-}" SONG_VERSION="$song_payload_version" /usr/bin/perl -MJSON::PP -e '
            my %r = (version => $ENV{SONG_VERSION}, status => $ENV{SONG_STATUS});
            $r{message} = $ENV{SONG_MESSAGE} if length $ENV{SONG_MESSAGE};
            $r{repair} = JSON::PP::true if $ENV{SONG_REPAIR};
            print JSON::PP->new->canonical->encode(\%r)' > "$song_result_file.tmp"
        /bin/mv -f "$song_result_file.tmp" "$song_result_file"
    }
    # OBS: the second argument, else the first installed copy Spotlight knows, else the defaults.
    if [[ $# -lt 2 ]]; then
        song_obs=''
        for song_candidate in ${(f)"$(/usr/bin/mdfind "kMDItemCFBundleIdentifier == 'com.obsproject.obs-studio'" 2>/dev/null)"} /Applications/OBS.app "$HOME/Applications/OBS.app"; do
            if [[ -f "$song_candidate/Contents/Info.plist" ]] && song_candidate_exe=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$song_candidate/Contents/Info.plist" 2>/dev/null) && [[ -f "$song_candidate/Contents/MacOS/$song_candidate_exe" ]]; then
                song_obs="$song_candidate"; break
            fi
        done
        [[ -n "$song_obs" ]] || { write_result failed 'OBS Studio was not found.'; song_success=1; exit; }
    fi
    take_lock update.lock || { song_success=1; exit; }
    song_obs_executable=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$song_obs/Contents/Info.plist")
    write_result waiting
    while true; do
        song_gd_running=0 song_obs_running=0
        /usr/bin/pgrep -x 'Geometry Dash' >/dev/null && song_gd_running=1
        /usr/bin/pgrep -x "$song_obs_executable" >/dev/null && song_obs_running=1
        if (( song_gd_running || song_obs_running )); then
            # "Update now" closed GD but OBS is open: give GD back now, install later.
            if (( ! song_gd_running )) && take_reopen_flag; then /usr/bin/open "$song_app" || true; fi
            sleep 2; continue
        fi
        song_log=$(/usr/bin/mktemp -t obs-jukebox-update)
        song_code=0
        /bin/zsh "$song_root/Install.command" --no-downgrade "$song_app" "$song_obs" </dev/null >"$song_log" 2>&1 || song_code=$?
        case $song_code in
            0) write_result installed; if take_reopen_flag; then /usr/bin/open "$song_app" || true; fi ;;
            2) sleep 2; continue ;;
            3) write_result failed "$(/usr/bin/tail -n 1 "$song_log")" repair ;;
            4) ;;
            *) write_result failed "$(/usr/bin/tail -n 1 "$song_log")" ;;
        esac
        song_success=1
        exit
    done
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
song_arch=$(/usr/bin/uname -m)
/usr/bin/lipo "$song_plugin" -verify_arch "$song_arch" >/dev/null 2>&1 || fail "This installer does not include a plugin for $song_arch."
/usr/bin/lipo "$song_obs/Contents/MacOS/$song_obs_executable" -verify_arch "$song_arch" >/dev/null 2>&1 || fail 'Install the native OBS Studio build for this Mac before continuing.'
take_lock install.lock || stop 2 'Another OBS Jukebox setup is running. Let it finish, then try again.'
if /usr/bin/pgrep -x 'Geometry Dash' >/dev/null || /usr/bin/pgrep -x "$song_obs_executable" >/dev/null; then
    stop 2 'Finish any OBS recording, close Geometry Dash and OBS, then click Install again.'
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
    [[ -d "$song_root/payload/geode/resources/geode.loader" ]] || fail 'The installer is missing Geode resources.'
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
my %versions;
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
        $versions{$id} = $mod->{version};
    }
    closedir $mods;
}
print(($installed{'fleym.nongd'} // '-'), "\n", ($installed{'local.separate_song'} // "$directory/local.separate_song.geode"), "\n", ($versions{'local.separate_song'} // '-'));
PERL
) || fail 'Installed mod compatibility could not be verified. No installation files have been changed.'
song_lines=("${(@f)song_packages}")
song_jukebox=${song_lines[1]} song_mod_destination=${song_lines[2]} song_installed_version=${song_lines[3]}
song_payload_version=$(/usr/bin/unzip -p "$song_root/payload/local.separate_song.geode" mod.json | /usr/bin/perl -MJSON::PP -e 'my $v = JSON::PP->new->relaxed->decode(join "", <STDIN>)->{version}; $v =~ s/\Av//; print $v')
if (( song_no_downgrade )) && version_newer "$song_installed_version" "$song_payload_version"; then
    stop 4 "A newer OBS Jukebox ($song_installed_version) is already installed."
fi
song_audit="$HOME/Library/Application Support/OBS Jukebox/Install Logs/$(/bin/date +%Y%m%d-%H%M%S)-$(/usr/bin/perl -MTime::HiRes=time -e 'printf "%.6f",time')-$(/usr/bin/uuidgen)"
/bin/mkdir -p "$song_audit/Backups" "$song_audit/Failed"
exec > >(/usr/bin/tee "$song_audit/install.log") 2>&1
song_version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$song_root/payload/separate-song.plugin/Contents/Info.plist")
print -r -- "OBS Jukebox $song_version" "Geometry Dash: $song_app" "OBS Studio: $song_obs" "Install log and backups: $song_audit"
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
    copy_item "$song_root/payload/geode/resources/geode.loader" "$song_app/Contents/geode/resources/geode.loader"
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
/usr/bin/touch "$song_audit/complete"
song_success=1
print -r -- 'Undo this installation with Install.command --uninstall.'
finish $'Installed!\n\n1. Open Geometry Dash and OBS.\n2. In OBS, add Sources > GD Sounds once. Keep Audio Monitoring off.\n3. In Jukebox, use the Game and OBS checkboxes to choose each song.\n4. In the pause menu, switch Game / OBS to adjust their volumes.\n\nGD Sounds includes game sound effects. Disable any duplicate GD audio capture in OBS.'
