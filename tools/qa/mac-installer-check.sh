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
touch "$qa_gd/Contents/MacOS/Geometry Dash"
qa_mods="$qa_gd/Contents/geode/mods"
qa_plugin="$qa_home/Library/Application Support/obs-studio/plugins/separate-song.plugin"
qa_logs="$qa_home/Library/Application Support/OBS Jukebox/Install Logs"
run_install() { /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" "$qa_gd" "${OBS_APP:-/Applications/OBS.app}" </dev/null; }
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
run_install > "$qa_root/first.log" 2>&1
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
run_install > "$qa_root/reinstall.log" 2>&1
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
run_install > "$qa_root/renamed.log" 2>&1
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
run_install > "$qa_root/replace.log" 2>&1
[[ ! -e "$qa_plugin/stale.txt" ]]
if /usr/bin/xattr -p com.apple.quarantine "$qa_plugin" >/dev/null 2>&1; then print 'Quarantine was copied'; exit 1; fi
print 'bundle-replaced-without-quarantine: pass'
# Undo restores the previous full bundle but preserves a subsequently edited mod.
print user-edited > "$qa_mods/local.separate_song.geode"
/usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" --uninstall > "$qa_root/undo.log" 2>&1
[[ "$(cat "$qa_mods/local.separate_song.geode")" == user-edited ]]
[[ "$(cat "$qa_plugin/stale.txt")" == stale ]]
print 'undo-restores-bundle-preserves-user-mod: pass'
