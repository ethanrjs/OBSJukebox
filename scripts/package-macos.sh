#!/bin/zsh
set -euo pipefail
song_root="${0:A:h:h}"
cd "$song_root"
: ${MAC_ARCH:=$(uname -m)}
: ${GEODE_PAYLOAD:?Set GEODE_PAYLOAD to the official macOS loader files and resources directory}
: ${JUKEBOX_PACKAGE:="$song_root/downloads/fleym.nongd.geode"}
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
cp scripts/check-geode-version.pl "$song_resources/check-geode-version.pl"
cp logo.png "$song_resources/logo.png"
cp "$MOD_PACKAGE" "$song_resources/payload/local.separate_song.geode"
cp "$JUKEBOX_PACKAGE" "$song_resources/payload/fleym.nongd.geode"
ditto dist/separate-song.plugin "$song_resources/payload/separate-song.plugin"
ditto "$GEODE_PAYLOAD" "$song_resources/payload/geode"
for song_path in src obs-plugin vendor scripts installer; do
    mkdir -p "$song_resources/Source/$song_path"
    tar -cf - --exclude=bin --exclude=obj --exclude=payload.zip "$song_path" | tar -xf - -C "$song_resources/Source"
done
cp CMakeLists.txt mod.json logo.png LICENSE README.md "$song_resources/Source/"
codesign --force --deep --sign - "$song_app"
song_archive="$song_root/artifacts/macos/OBS-Jukebox-$song_version-macOS-$MAC_ARCH.zip"
ditto -c -k --sequesterRsrc --keepParent "$song_app" "$song_archive"
shasum -a 256 "$song_archive"
