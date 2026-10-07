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
print 'original-geode' > "$qa_gd/Contents/Frameworks/Geode.dylib"
print 'original-fmod' > "$qa_gd/Contents/Frameworks/libfmod.dylib"
touch "$qa_gd/Contents/MacOS/Geometry Dash"
qa_mods="$qa_gd/Contents/geode/mods"
qa_plugin="$qa_home/Library/Application Support/obs-studio/plugins/separate-song.plugin"
qa_logs="$qa_home/Library/Application Support/OBS Jukebox/Install Logs"
run_install() { /usr/bin/env HOME="$qa_home" /bin/zsh "$qa_script" "$qa_gd" /Applications/OBS.app </dev/null; }
run_install > "$qa_root/first.log" 2>&1
cmp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/local.separate_song.geode"
cmp "$qa_payload/payload/fleym.nongd.geode" "$qa_mods/fleym.nongd.geode"
cmp "$qa_payload/payload/separate-song.plugin/Contents/MacOS/separate-song" "$qa_plugin/Contents/MacOS/separate-song"
[[ "$(cat "$qa_gd/Contents/Frameworks/Geode.dylib")" == original-geode ]]
[[ "$(cat "$qa_gd/Contents/Frameworks/libfmod.dylib")" == original-fmod ]]
print 'first-install: pass'
print old-mod > "$qa_mods/local.separate_song.geode"
print old-jukebox > "$qa_mods/fleym.nongd.geode"
print old-plugin > "$qa_plugin/Contents/MacOS/separate-song"
run_install > "$qa_root/reinstall.log" 2>&1
qa_backups=("$qa_logs"/*/Backups)
qa_found=0
for qa_backup in "${qa_backups[@]}"; do
    if [[ -f "$qa_backup/1" && "$(cat "$qa_backup/1")" == old-jukebox ]]; then
        [[ "$(cat "$qa_backup/2")" == old-mod ]]
        [[ "$(cat "$qa_backup/3/Contents/MacOS/separate-song")" == old-plugin ]]
        qa_found=1
    fi
done
[[ "$qa_found" == 1 ]]
cmp "$qa_payload/payload/local.separate_song.geode" "$qa_mods/local.separate_song.geode"
cmp "$qa_payload/payload/separate-song.plugin/Contents/MacOS/separate-song" "$qa_plugin/Contents/MacOS/separate-song"
print 'reinstall-backups: pass'
qa_before=$(shasum -a 256 "$qa_mods/local.separate_song.geode" "$qa_mods/fleym.nongd.geode" "$qa_plugin/Contents/MacOS/separate-song" "$qa_gd/Contents/Frameworks/Geode.dylib" "$qa_gd/Contents/Frameworks/libfmod.dylib")
mv "$qa_payload/payload/local.separate_song.geode" "$qa_root/held.geode"
if run_install > "$qa_root/missing-payload.log" 2>&1; then print 'Missing payload incorrectly accepted'; exit 1; fi
qa_after=$(shasum -a 256 "$qa_mods/local.separate_song.geode" "$qa_mods/fleym.nongd.geode" "$qa_plugin/Contents/MacOS/separate-song" "$qa_gd/Contents/Frameworks/Geode.dylib" "$qa_gd/Contents/Frameworks/libfmod.dylib")
[[ "$qa_before" == "$qa_after" ]]
print 'missing-payload-preserves-install: pass'
mv "$qa_root/held.geode" "$qa_payload/payload/local.separate_song.geode"
print rollback-mod > "$qa_mods/local.separate_song.geode"
print rollback-jukebox > "$qa_mods/fleym.nongd.geode"
mv "$qa_plugin" "$qa_root/held-plugin"
ln -s "$qa_root/held-plugin" "$qa_plugin"
if run_install > "$qa_root/rollback.log" 2>&1; then print 'Symbolic-link target incorrectly accepted'; exit 1; fi
[[ "$(cat "$qa_mods/local.separate_song.geode")" == rollback-mod ]]
[[ "$(cat "$qa_mods/fleym.nongd.geode")" == rollback-jukebox ]]
[[ -L "$qa_plugin" ]]
[[ "$(cat "$qa_gd/Contents/Frameworks/Geode.dylib")" == original-geode ]]
print 'failed-install-rolls-back: pass'
print -r -- "Evidence: $qa_root"
