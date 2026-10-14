# Merges the Windows and macOS packages into the one .geode the Geode index takes, checks it, and
# writes its SHA-256 next to it for the release notes.
[CmdletBinding()]
param([string]$Cli = (Join-Path $PSScriptRoot '../tools/pinned/geode-cli/geode.exe'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'artifacts/geode'
New-Item -ItemType Directory -Force $out | Out-Null
$merged = Join-Path $out 'local.separate_song.geode'
# geode package merge writes into its first argument, and takes mod.json, about.md and the logo from it.
Copy-Item (Join-Path $root 'artifacts/windows/local.separate_song.geode') $merged -Force
& $Cli package merge $merged (Join-Path $root 'artifacts/macos/local.separate_song.geode')
if ($LASTEXITCODE) { exit $LASTEXITCODE }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($merged)
try { $names = @($zip.Entries.FullName) } finally { $zip.Dispose() }
foreach ($entry in 'mod.json', 'about.md', 'logo.png', 'local.separate_song.dll', 'local.separate_song.dylib') {
    if ($names -notcontains $entry) { throw "The merged package has no $entry." }
}
$hash = (Get-FileHash $merged -Algorithm SHA256).Hash.ToLower()
"$hash  local.separate_song.geode" | Set-Content (Join-Path $out 'SHA256SUMS.txt') -Encoding ascii
"Merged package SHA-256: $hash"
