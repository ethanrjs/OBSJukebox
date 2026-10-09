#!/bin/zsh
set -euo pipefail
song_root="${0:A:h:h}"
cd "$song_root"
: ${MAC_ARCH:=$(uname -m)}
: ${DEPENDENCY_ROOT:="$song_root/tools/pinned"}
python3 scripts/fetch-dependencies.py mac --destination "$DEPENDENCY_ROOT"
: ${GEODE_PAYLOAD:="$DEPENDENCY_ROOT/geode-macos-5.10.1"}
: ${JUKEBOX_PACKAGE:="$DEPENDENCY_ROOT/fleym.nongd.geode"}
: ${MOD_PACKAGE:="$song_root/build-mac/local.separate_song.geode"}
song_version=$(/usr/bin/plutil -extract version raw -o - mod.json)
song_version="${song_version#v}"
mkdir -p "$song_root/artifacts"
song_stage=$(mktemp -d "$song_root/artifacts/macos-package.XXXXXX")
trap 'rm -rf -- "$song_stage"' EXIT
song_app="$song_stage/OBS Jukebox Setup.app"
song_resources="$song_app/Contents/Resources"
mkdir -p "$song_app/Contents/MacOS" "$song_resources/payload" "$song_resources/Source" "$song_root/artifacts/macos"
swiftc -O -parse-as-library -target "$MAC_ARCH-apple-macos13.0" installer/Installer.swift -o "$song_app/Contents/MacOS/OBSJukeboxSetup"
cat > "$song_app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>OBSJukeboxSetup</string>
<key>CFBundleIdentifier</key><string>local.obsjukebox.setup</string>
<key>CFBundleName</key><string>OBS Jukebox Setup</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$song_version</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
cp scripts/Install.command "$song_resources/Install.command"
cp scripts/install-state.pl "$song_resources/install-state.pl"
cp scripts/check-geode-version.pl "$song_resources/check-geode-version.pl"
cp logo.png "$song_resources/logo.png"
cp "$MOD_PACKAGE" "$song_resources/payload/local.separate_song.geode"
cp "$JUKEBOX_PACKAGE" "$song_resources/payload/fleym.nongd.geode"
cp vendor/jukebox-3.8.0/LICENSE "$song_resources/Jukebox-LICENSE"
cp LICENSE "$song_resources/LICENSE"
ditto dist/separate-song.plugin "$song_resources/payload/separate-song.plugin"
ditto "$GEODE_PAYLOAD" "$song_resources/payload/geode"
cp "$DEPENDENCY_ROOT/geode-sdk/loader/include/link/macos/libfmod.dylib" "$song_resources/payload/geode/libfmod.dylib"
mkdir -p "$song_resources/payload/geode/resources/geode.loader"
ditto "$DEPENDENCY_ROOT/geode-resources" "$song_resources/payload/geode/resources/geode.loader"
for song_path in src obs-plugin vendor scripts installer tools/qa .github; do
    mkdir -p "$song_resources/Source/$song_path"
    tar -cf - --exclude=bin --exclude=obj --exclude=payload.zip "$song_path" | tar -xf - -C "$song_resources/Source"
done
cp CMakeLists.txt mod.json logo.png LICENSE README.md "$song_resources/Source/"
codesign --force --deep --sign - "$song_app"
song_archive="$song_root/artifacts/macos/OBS-Jukebox-$song_version-macOS-$MAC_ARCH.zip"
ditto -c -k --sequesterRsrc --keepParent "$song_app" "$song_archive"
shasum -a 256 "$song_archive"
