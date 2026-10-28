#!/bin/zsh
set -euo pipefail
[[ $# -eq 1 ]] || { print 'Usage: mac-installer-check.sh path/to/package.zip'; exit 2; }
qa_root=$(mktemp -d /tmp/obs-jukebox-installer.XXXXXX)
qa_home="$qa_root/home"
qa_gd="$qa_root/Geometry Dash.app"
mkdir -p "$qa_home" "$qa_gd/Contents/MacOS" "$qa_gd/Contents/Frameworks"
unzip -q "$1" -d "$qa_root/package"
qa_payload="$qa_root/package/OBS Jukebox Setup.app/Contents/Resources"
qa_script="$qa_payload/Install.command"
cp "$qa_payload/payload/geode/Geode.dylib" "$qa_gd/Contents/Frameworks/Geode.dylib"
print 'original-fmod' > "$qa_gd/Contents/Frameworks/libfmod.dylib"
# A launchable fake GD: the reopen flag test checks that the wrapper actually opens it.
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<plist version="1.0"><dict>' '<key>CFBundleExecutable</key><string>Geometry Dash</string>' '<key>CFBundleIdentifier</key><string>com.example.geometrydash</string>' '<key>CFBundleName</key><string>Geometry Dash</string>' '<key>CFBundlePackageType</key><string>APPL</string>' '</dict></plist>' > "$qa_gd/Contents/Info.plist"
printf '#!/bin/sh\necho launched >> "%s"\n' "$qa_root/reopened" > "$qa_gd/Contents/MacOS/Geometry Dash"
chmod +x "$qa_gd/Contents/MacOS/Geometry Dash"
qa_mods="$qa_gd/Contents/geode/mods"
qa_plugin="$qa_home/Library/Application Support/obs-studio/plugins/separate-song.plugin"
qa_logs="$qa_home/Library/Application Support/OBS Jukebox/Install Logs"
run_install() { /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" "$qa_gd" "${OBS_APP:-/Applications/OBS.app}" </dev/null; }
# For an install that should succeed. The runner discards qa_root, so if it fails, print its log.
install_ok() {
    run_install > "$qa_root/$1.log" 2>&1 && return
    print -r -- "$1: the installer failed. Its log:"
    cat "$qa_root/$1.log"
    exit 1
}
make_mod() {
    local destination="$1" id="$2" version="$3" platform="${4:-mac}"
    local staging=$(mktemp -d "$qa_root/mod.XXXXXX")
    print -r -- "{\"id\":\"$id\",\"version\":\"$version\"}" > "$staging/mod.json"
    if [[ "$platform" == mac ]]; then touch "$staging/$id.dylib"; fi
    (cd "$staging" && /usr/bin/zip -q "$destination" *)
}
reject_mods() {
    local name="$1"
    local before=$(find "$qa_mods" -type f -exec shasum -a 256 {} \; | sort)
    if run_install > "$qa_root/$name.log" 2>&1; then print "$name incorrectly accepted"; exit 1; fi
    local after=$(find "$qa_mods" -type f -exec shasum -a 256 {} \; | sort)
    [[ "$before" == "$after" && ! -e "$qa_logs" && ! -e "$qa_plugin" ]]
    print -r -- "$name: pass"
}
for qa_version in 5.10.0 5.9.9 4.10.1 6.10.1 invalid 5.10.1-beta.1; do
    /usr/bin/env QA_VERSION="$qa_version" /usr/bin/perl -0pe 's/"5\.10\.1"/"$ENV{QA_VERSION}"/g' "$qa_payload/payload/geode/Geode.dylib" > "$qa_gd/Contents/Frameworks/Geode.dylib"
    qa_before=$(shasum -a 256 "$qa_gd/Contents/Frameworks/Geode.dylib" "$qa_gd/Contents/Frameworks/libfmod.dylib")
    if run_install > "$qa_root/reject-$qa_version.log" 2>&1; then print "Incompatible Geode $qa_version incorrectly accepted"; exit 1; fi
    qa_after=$(shasum -a 256 "$qa_gd/Contents/Frameworks/Geode.dylib" "$qa_gd/Contents/Frameworks/libfmod.dylib")
    [[ "$qa_before" == "$qa_after" && ! -e "$qa_logs" && ! -e "$qa_mods" && ! -e "$qa_plugin" ]]
done
print 'incompatible-geode-rejected-before-mutation: pass'
cp "$qa_payload/payload/geode/Geode.dylib" "$qa_gd/Contents/Frameworks/Geode.dylib"
mkdir -p "$qa_mods"
make_mod "$qa_mods/renamed.geode" fleym.nongd 3.7.0
reject_mods incompatible-renamed-jukebox
rm "$qa_mods/renamed.geode"
make_mod "$qa_mods/renamed.geode" fleym.nongd 3.8.0 windows
reject_mods jukebox-without-macos
rm "$qa_mods/renamed.geode"
cp "$qa_payload/payload/fleym.nongd.geode" "$qa_mods/renamed.geode"
cp "$qa_payload/payload/fleym.nongd.geode" "$qa_mods/duplicate.geode"
reject_mods duplicate-jukebox
rm "$qa_mods/renamed.geode" "$qa_mods/duplicate.geode"
cp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/renamed.geode"
cp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/duplicate.geode"
reject_mods duplicate-obs-jukebox
rm "$qa_mods/renamed.geode" "$qa_mods/duplicate.geode"
print invalid > "$qa_mods/fleym.nongd.geode"
reject_mods invalid-canonical-package
rm "$qa_mods/fleym.nongd.geode"
make_mod "$qa_mods/local.separate_song.geode" other.mod 1.0.0
reject_mods occupied-canonical-package
rm "$qa_mods/local.separate_song.geode"
make_mod "$qa_mods/LOCAL.SEPARATE_SONG.geode" other.mod 1.0.0
reject_mods occupied-canonical-package-case
rm "$qa_mods/LOCAL.SEPARATE_SONG.geode"
ln -s "$qa_payload/payload/fleym.nongd.geode" "$qa_mods/renamed.geode"
reject_mods renamed-jukebox-symbolic-link
rm "$qa_mods/renamed.geode"
print unrelated-invalid > "$qa_mods/other.geode"
install_ok first
cmp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/local.separate_song.geode"
cmp "$qa_payload/payload/fleym.nongd.geode" "$qa_mods/fleym.nongd.geode"
cmp "$qa_payload/payload/separate-song.plugin/Contents/MacOS/separate-song" "$qa_plugin/Contents/MacOS/separate-song"
cmp "$qa_payload/payload/geode/Geode.dylib" "$qa_gd/Contents/Frameworks/Geode.dylib"
[[ "$(cat "$qa_gd/Contents/Frameworks/libfmod.dylib")" == original-fmod ]]
print 'first-install: pass'
[[ "$(cat "$qa_mods/other.geode")" == unrelated-invalid ]]
print 'unrelated-malformed-package-preserved: pass'
rm "$qa_mods/local.separate_song.geode"
make_mod "$qa_mods/local.separate_song.geode" local.separate_song 0.9.0
cp "$qa_mods/local.separate_song.geode" "$qa_root/old-mod.geode"
print old-plugin > "$qa_plugin/Contents/MacOS/separate-song"
install_ok reinstall
qa_backups=("$qa_logs"/*/Backups)
qa_found=0
for qa_backup in "${qa_backups[@]}"; do
    if [[ -f "$qa_backup/1" ]] && cmp -s "$qa_backup/1" "$qa_root/old-mod.geode"; then
        [[ "$(cat "$qa_backup/2/Contents/MacOS/separate-song")" == old-plugin ]]
        qa_found=1
    fi
done
[[ "$qa_found" == 1 ]]
cmp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/local.separate_song.geode"
cmp "$qa_payload/payload/separate-song.plugin/Contents/MacOS/separate-song" "$qa_plugin/Contents/MacOS/separate-song"
print 'reinstall-backups: pass'
mv "$qa_mods/fleym.nongd.geode" "$qa_mods/my-jukebox.geode"
mv "$qa_mods/local.separate_song.geode" "$qa_mods/my-obs-jukebox.geode"
install_ok renamed
[[ ! -e "$qa_mods/fleym.nongd.geode" && ! -e "$qa_mods/local.separate_song.geode" ]]
cmp "$qa_payload/payload/fleym.nongd.geode" "$qa_mods/my-jukebox.geode"
cmp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/my-obs-jukebox.geode"
print 'renamed-packages-preserved: pass'
mv "$qa_mods/my-jukebox.geode" "$qa_mods/fleym.nongd.geode"
mv "$qa_mods/my-obs-jukebox.geode" "$qa_mods/local.separate_song.geode"
qa_before=$(shasum -a 256 "$qa_mods/local.separate_song.geode" "$qa_mods/fleym.nongd.geode" "$qa_plugin/Contents/MacOS/separate-song" "$qa_gd/Contents/Frameworks/Geode.dylib" "$qa_gd/Contents/Frameworks/libfmod.dylib")
mv "$qa_payload/payload/local.separate_song.geode" "$qa_root/held.geode"
if run_install > "$qa_root/missing-payload.log" 2>&1; then print 'Missing payload incorrectly accepted'; exit 1; fi
qa_after=$(shasum -a 256 "$qa_mods/local.separate_song.geode" "$qa_mods/fleym.nongd.geode" "$qa_plugin/Contents/MacOS/separate-song" "$qa_gd/Contents/Frameworks/Geode.dylib" "$qa_gd/Contents/Frameworks/libfmod.dylib")
[[ "$qa_before" == "$qa_after" ]]
print 'missing-payload-preserves-install: pass'
mv "$qa_root/held.geode" "$qa_payload/payload/local.separate_song.geode"
cp "$qa_root/old-mod.geode" "$qa_mods/local.separate_song.geode"
mv "$qa_plugin" "$qa_root/held-plugin"
ln -s "$qa_root/held-plugin" "$qa_plugin"
if run_install > "$qa_root/rollback.log" 2>&1; then print 'Symbolic-link target incorrectly accepted'; exit 1; fi
cmp "$qa_mods/local.separate_song.geode" "$qa_root/old-mod.geode"
cmp "$qa_mods/fleym.nongd.geode" "$qa_payload/payload/fleym.nongd.geode"
[[ -L "$qa_plugin" ]]
cmp "$qa_payload/payload/geode/Geode.dylib" "$qa_gd/Contents/Frameworks/Geode.dylib"
print 'failed-install-rolls-back: pass'
print -r -- "Evidence: $qa_root"

# A new install must remove old bundle members and quarantine from the payload.
rm "$qa_plugin"
mv "$qa_root/held-plugin" "$qa_plugin"
print stale > "$qa_plugin/stale.txt"
/usr/bin/xattr -w com.apple.quarantine '0081;00000000;Fixture;' "$qa_payload/payload/separate-song.plugin"
install_ok replace
[[ ! -e "$qa_plugin/stale.txt" ]]
if /usr/bin/xattr -p com.apple.quarantine "$qa_plugin" >/dev/null 2>&1; then print 'Quarantine was copied'; exit 1; fi
print 'bundle-replaced-without-quarantine: pass'
# Undo restores the previous full bundle but preserves a subsequently edited mod.
print user-edited > "$qa_mods/local.separate_song.geode"
/usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" --uninstall > "$qa_root/undo.log" 2>&1
[[ "$(cat "$qa_mods/local.separate_song.geode")" == user-edited ]]
[[ "$(cat "$qa_plugin/stale.txt")" == stale ]]
print 'undo-restores-bundle-preserves-user-mod: pass'
# A Mac without Geode gets Geode's resources directly in geode.loader, not one folder deeper.
qa_fresh="$qa_root/Fresh Geometry Dash.app"
mkdir -p "$qa_fresh/Contents/MacOS" "$qa_fresh/Contents/Frameworks"
print 'original-fmod' > "$qa_fresh/Contents/Frameworks/libfmod.dylib"
touch "$qa_fresh/Contents/MacOS/Geometry Dash"
if ! /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" "$qa_fresh" "${OBS_APP:-/Applications/OBS.app}" </dev/null > "$qa_root/fresh.log" 2>&1; then
    print 'fresh: the installer failed. Its log:'; cat "$qa_root/fresh.log"; exit 1
fi
qa_resources=("$qa_payload/payload/geode/resources/geode.loader"/*(N))
(( ${#qa_resources} > 0 ))
[[ -f "$qa_fresh/Contents/geode/resources/geode.loader/${qa_resources[1]:t}" ]]
[[ ! -e "$qa_fresh/Contents/geode/resources/geode.loader/geode.loader" ]]
print 'fresh-geode-resources-at-top-level: pass'
# Exit codes the update wrapper relies on. Start from a complete install of the packaged mod.
cp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/local.separate_song.geode"
qa_lock_dir="$qa_home/Library/Application Support/OBS Jukebox"
mkdir -p "$qa_lock_dir"; : >> "$qa_lock_dir/install.lock"
/bin/zsh -c 'zmodload zsh/system; zsystem flock "$1"; sleep 6' qa-holder "$qa_lock_dir/install.lock" & qa_holder=$!
sleep 1
qa_code=0; run_install > "$qa_root/locked.log" 2>&1 || qa_code=$?
wait $qa_holder
(( qa_code == 2 )) || { print "A held install lock gave $qa_code"; exit 1; }
print 'install-lock-busy-exit-2: pass'
cp "$qa_mods/local.separate_song.geode" "$qa_root/current-mod.geode"
make_mod "$qa_mods/local.separate_song.geode" local.separate_song v9.9.9
qa_code=0; /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" --no-downgrade "$qa_gd" "${OBS_APP:-/Applications/OBS.app}" </dev/null > "$qa_root/newer.log" 2>&1 || qa_code=$?
(( qa_code == 4 )) || { print "A newer installed mod gave $qa_code"; exit 1; }
[[ "$(unzip -p "$qa_mods/local.separate_song.geode" mod.json)" == *9.9.9* ]]
cp "$qa_root/current-mod.geode" "$qa_mods/local.separate_song.geode"
print 'no-downgrade-exit-4: pass'
# A command that fails mid-install with its own status 2 must still end as 1, after a full rollback.
# install-state.pl runs after each copy, so a stand-in that fails makes a deterministic mid-install error.
cp "$qa_payload/install-state.pl" "$qa_root/install-state.pl.real"
# The mod must be a different version, so a passing comparison proves the rollback put it back.
make_mod "$qa_mods/local.separate_song.geode" local.separate_song 0.9.0
cp "$qa_mods/local.separate_song.geode" "$qa_root/before-failure.geode"
print -r -- '#!/usr/bin/perl
exit 2;' > "$qa_payload/install-state.pl"
qa_code=0; run_install > "$qa_root/raw-two.log" 2>&1 || qa_code=$?
(( qa_code == 1 )) || { print "A command exiting 2 mid-install gave $qa_code"; cat "$qa_root/raw-two.log"; exit 1; }
cmp "$qa_mods/local.separate_song.geode" "$qa_root/before-failure.geode"
print 'raw-status-normalized-exit-1: pass'
# The same failure, after deleting this run's backups, leaves the rollback unable to finish: exit 3.
print -r -- '#!/usr/bin/perl
use File::Path qw(remove_tree);
remove_tree("$ARGV[1]/Backups") if $ARGV[0] eq "record";
exit 2;' > "$qa_payload/install-state.pl"
qa_code=0; run_install > "$qa_root/repair.log" 2>&1 || qa_code=$?
cp "$qa_root/install-state.pl.real" "$qa_payload/install-state.pl"
(( qa_code == 3 )) || { print "An unfinished rollback gave $qa_code"; cat "$qa_root/repair.log"; exit 1; }
cp "$qa_root/before-failure.geode" "$qa_mods/local.separate_song.geode"
print 'unfinished-rollback-exit-3: pass'
qa_code=0; /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" --uninstall </dev/null > "$qa_root/undo-code.log" 2>&1 || qa_code=$?
(( qa_code == 0 )) || { print "A successful undo gave $qa_code"; exit 1; }
print 'undo-exit-0: pass'
# The update wrapper: waits for GD and OBS, installs, reports.
qa_result="$qa_root/update-result.json"
qa_status() { /usr/bin/perl -MJSON::PP -e 'local $/; open my $f, "<", shift or exit 1; print decode_json(<$f>)->{status}' "$qa_result"; }
run_update() { /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" --update --result "$qa_result" --reopen-flag "$qa_root/reopen" "$qa_gd" "${OBS_APP:-/Applications/OBS.app}" </dev/null; }
cp /bin/sleep "$qa_root/Geometry Dash"
"$qa_root/Geometry Dash" 20 & qa_fake_gd=$!
run_update > "$qa_root/update.log" 2>&1 & qa_wrapper=$!
sleep 3
[[ "$(qa_status)" == waiting ]] || { print 'The wrapper did not report waiting'; cat "$qa_root/update.log"; exit 1; }
run_update > "$qa_root/update-second.log" 2>&1
[[ "$(qa_status)" == waiting ]]
print 'update-second-wrapper-exits: pass'
kill $qa_fake_gd; wait $qa_fake_gd 2>/dev/null || true
wait $qa_wrapper
[[ "$(qa_status)" == installed ]] || { print 'The wrapper did not install'; cat "$qa_root/update.log"; exit 1; }
print 'update-waits-then-installs: pass'
rm -f "$qa_root/reopened"
: > "$qa_root/reopen"
run_update > "$qa_root/update-reopen.log" 2>&1
for qa_try in {1..30}; do [[ -e "$qa_root/reopened" ]] && break; sleep 0.1; done
[[ "$(qa_status)" == installed && ! -e "$qa_root/reopen" && -e "$qa_root/reopened" ]]
print 'update-reopen-flag-consumed: pass'
/bin/zsh -c 'zmodload zsh/system; : >> "$1"; zsystem flock "$1"; sleep 4' qa-holder "$qa_lock_dir/update.lock" & qa_holder=$!
sleep 1
rm -f "$qa_result"
run_update > /dev/null 2>&1
[[ ! -e "$qa_result" ]]
wait $qa_holder
run_update > /dev/null 2>&1
[[ "$(qa_status)" == installed ]]
print 'update-lock-free-after-holder-exits: pass'
# A killed wrapper leaves no stale lock: the kernel drops an fcntl lock with its process.
/bin/zsh -c 'zmodload zsh/system; : >> "$1"; zsystem flock "$1"; sleep 60' qa-holder "$qa_lock_dir/update.lock" & qa_holder=$!
sleep 1
kill -9 $qa_holder; wait $qa_holder 2>/dev/null || true
rm -f "$qa_result"
run_update > /dev/null 2>&1
[[ "$(qa_status)" == installed ]]
print 'update-lock-free-after-kill: pass'
# An unfinished rollback reaches the mod as failed with repair.
print -r -- '#!/usr/bin/perl
use File::Path qw(remove_tree);
remove_tree("$ARGV[1]/Backups") if $ARGV[0] eq "record";
exit 2;' > "$qa_payload/install-state.pl"
run_update > /dev/null 2>&1
cp "$qa_root/install-state.pl.real" "$qa_payload/install-state.pl"
cp "$qa_root/before-failure.geode" "$qa_mods/local.separate_song.geode"
[[ "$(qa_status)" == failed ]] && /usr/bin/perl -MJSON::PP -e 'local $/; open my $f, "<", shift or exit 1; exit(decode_json(<$f>)->{repair} ? 0 : 1)' "$qa_result"
print 'update-repair-result: pass'
