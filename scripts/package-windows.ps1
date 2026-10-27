[CmdletBinding()]
param(
    [string]$ModPath,
    [string]$PluginPath,
    [string]$GeodeRoot,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$metadata = Get-Content -LiteralPath (Join-Path $projectRoot 'mod.json') -Raw | ConvertFrom-Json
$releaseVersion = $metadata.version.TrimStart('v')
if ($releaseVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'mod.json must contain a release version such as v1.2.0.' }
if (!$ModPath) { $ModPath = Join-Path $projectRoot 'artifacts/windows/local.separate_song.geode' }
if (!$PluginPath) { $PluginPath = Join-Path $projectRoot 'artifacts/windows/separate-song.dll' }
if (!$GeodeRoot) { $GeodeRoot = Join-Path $projectRoot ('tools/pinned/geode-windows-' + $metadata.geode.TrimStart('v')) }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $projectRoot 'artifacts/windows/release' }
foreach ($file in @($ModPath, $PluginPath)) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Payload not found: $file" }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$modArchive = [System.IO.Compression.ZipFile]::OpenRead($ModPath)
try {
    $entry = $modArchive.GetEntry('mod.json')
    if (!$entry) { throw 'The mod package is missing mod.json.' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $packagedMetadata = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    if ($packagedMetadata.id -ne $metadata.id -or $packagedMetadata.version.TrimStart('v') -ne $releaseVersion) {
        throw "The mod package does not match mod.json ($($metadata.id) $releaseVersion). Rebuild the mod before packaging."
    }
} finally { $modArchive.Dispose() }
if (!(Test-Path -LiteralPath (Join-Path $GeodeRoot 'geode/resources/geode.loader'))) { throw 'Complete official Geode 5.10.1 resources are required.' }
$staging = Join-Path $projectRoot ('artifacts/windows/package-' + [Guid]::NewGuid().ToString('N'))
$installerRoot = Join-Path $projectRoot 'installer/windows'
New-Item -ItemType Directory -Path $staging, $OutputDirectory -Force | Out-Null
try {
    $payloadRoot = Join-Path $staging 'payload'
    New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'mods'), (Join-Path $payloadRoot 'obs'), (Join-Path $payloadRoot 'geode') -Force | Out-Null
    Copy-Item -LiteralPath $ModPath -Destination (Join-Path $payloadRoot 'mods/local.separate_song.geode')
    Copy-Item -LiteralPath $PluginPath -Destination (Join-Path $payloadRoot 'obs/separate-song.dll')
    foreach ($name in @('Geode.dll', 'GeodeUpdater.exe', 'XInput1_4.dll')) { Copy-Item -LiteralPath (Join-Path $GeodeRoot $name) -Destination (Join-Path $payloadRoot 'geode') }
    Copy-Item -LiteralPath (Join-Path $GeodeRoot 'geode') -Destination (Join-Path $payloadRoot 'geode/geode') -Recurse
    $embedded = Join-Path $installerRoot 'payload.zip'
    if (Test-Path -LiteralPath $embedded) { Remove-Item -LiteralPath $embedded }
    [System.IO.Compression.ZipFile]::CreateFromDirectory($payloadRoot, $embedded, [System.IO.Compression.CompressionLevel]::Optimal, $false)
    $publish = Join-Path $staging 'publish'
    & dotnet publish (Join-Path $installerRoot 'SeparateSongSetup.csproj') -c Release -r win-x64 --self-contained true -o $publish --nologo "-p:Version=$releaseVersion" '-p:PublishSingleFile=true' '-p:EnableCompressionInSingleFile=true' '-p:IncludeNativeLibrariesForSelfExtract=true' '-p:DebugType=None' '-p:DebugSymbols=false'
    if ($LASTEXITCODE -ne 0) { throw "Installer publish failed: $LASTEXITCODE" }
    $exe = Join-Path $OutputDirectory "OBS-Jukebox-$releaseVersion-Windows-Setup.exe"
    Copy-Item -LiteralPath (Join-Path $publish 'OBSJukeboxSetup.exe') -Destination $exe -Force
    $hashes = [ordered]@{}
    foreach ($file in @($exe, $ModPath, $PluginPath)) { $hashes[(Split-Path $file -Leaf)] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }
    $hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256.json') -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination $OutputDirectory -Force
    # Source comes from the commit, so untracked files in a developer's tree never ship.
    if (& git -C $projectRoot status --porcelain) { Write-Warning 'Source is taken from HEAD; uncommitted changes are not in it.' }
    $source = Join-Path $OutputDirectory 'Source'
    if (Test-Path -LiteralPath $source) { Remove-Item -LiteralPath $source -Recurse -Force }
    $sourceArchive = Join-Path $staging 'source.zip'
    & git -C $projectRoot archive --format=zip -o $sourceArchive HEAD -- src obs-plugin vendor installer scripts tools/qa .github CMakeLists.txt mod.json logo.png LICENSE README.md
    if ($LASTEXITCODE -ne 0) { throw "git archive failed: $LASTEXITCODE" }
    Expand-Archive -LiteralPath $sourceArchive -DestinationPath $source
    Write-Output "Installer: $exe"
    Write-Output ('SHA256: ' + $hashes[(Split-Path $exe -Leaf)])
} finally {
    $resolvedStaging = [IO.Path]::GetFullPath($staging)
    $allowed = [IO.Path]::GetFullPath((Join-Path $projectRoot 'artifacts/windows')) + [IO.Path]::DirectorySeparatorChar
    if (!$resolvedStaging.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside artifacts/windows.' }
    Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
}
