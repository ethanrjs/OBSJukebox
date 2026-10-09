param(
    [string]$ObsDirectory = "$env:ProgramFiles\obs-studio",
    [string]$GeodeSdk,
    [string]$DependencyRoot,
    [string]$Configuration = 'Release',
    [int]$Parallel = 4,
    [switch]$SkipMod
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (!$DependencyRoot) { $DependencyRoot = Join-Path $root 'tools/pinned' }
& python (Join-Path $PSScriptRoot 'fetch-dependencies.py') windows --destination $DependencyRoot
if ($LASTEXITCODE) { throw 'Dependency verification failed.' }
$buildRoot = Join-Path $root 'build-windows'
$output = Join-Path $root 'artifacts\windows'
New-Item -ItemType Directory -Force -Path $buildRoot,$output | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio 2022 C++ Build Tools are required.' }
$msvc = Get-ChildItem -LiteralPath "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$nativeTools = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$requiredSdkVersion = (Get-Content -LiteralPath (Join-Path $root 'mod.json') -Raw | ConvertFrom-Json).geode.TrimStart('v')
if (!$GeodeSdk) { $GeodeSdk = Join-Path $DependencyRoot 'geode-sdk' }
$sdk = (Resolve-Path -LiteralPath $GeodeSdk).Path
$sdkVersion = (Get-Content -LiteralPath (Join-Path $sdk 'VERSION') -Raw).Trim().TrimStart('v')
if ($sdkVersion -ne $requiredSdkVersion) { throw "Expected SDK $requiredSdkVersion, found $sdkVersion" }
$loaderDirectory = Join-Path $DependencyRoot "geode-windows-$sdkVersion"
$loaderResources = Join-Path $loaderDirectory 'geode/resources/geode.loader'
New-Item -ItemType Directory -Force -Path $loaderResources | Out-Null
Copy-Item -Path (Join-Path $DependencyRoot 'geode-resources/*') -Destination $loaderResources -Recurse -Force
$sdkBinaryDirectory = Join-Path $sdk "bin\$sdkVersion"
New-Item -ItemType Directory -Force -Path $sdkBinaryDirectory | Out-Null
Copy-Item -LiteralPath "$loaderDirectory\Geode.lib" -Destination "$sdkBinaryDirectory\Geode.lib" -Force
if (!$SkipMod) {
    $env:GEODE_SDK = $sdk.Replace('\','/')
    & cmake -S $root -B "$buildRoot\mod" -G 'Visual Studio 17 2022' -A x64 "-DGEODE_BINDINGS_REPO_PATH=$DependencyRoot/bindings" "-DGEODE_CLI=$DependencyRoot/geode-cli/geode.exe" '-DGEODE_DISABLE_PRECOMPILED_HEADERS=ON' '-DOBS_JUKEBOX_QA=OFF'
    if ($LASTEXITCODE) { throw 'Mod configuration failed.' }
    & cmake --build "$buildRoot\mod" --config $Configuration --parallel $Parallel
    if ($LASTEXITCODE) { throw 'Mod compilation failed.' }
    $mod = Get-ChildItem -LiteralPath "$buildRoot\mod" -Filter local.separate_song.geode -Recurse | Select-Object -First 1
    if (!$mod) { throw 'Geode package was not produced.' }
    Copy-Item -LiteralPath $mod.FullName -Destination "$output\local.separate_song.geode" -Force
}
$obsDll = Join-Path $ObsDirectory 'bin\64bit\obs.dll'
if (!(Test-Path -LiteralPath $obsDll)) { throw "OBS DLL not found at $obsDll" }
$exports = & "$nativeTools\dumpbin.exe" /exports $obsDll
if ($LASTEXITCODE) { throw 'Could not read OBS DLL exports.' }
$names = @($exports | ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $Matches[1] } })
if ($names.Count -lt 100) { throw 'Unexpected OBS DLL export list.' }
$def = Join-Path $buildRoot 'obs.def'
@('LIBRARY obs.dll','EXPORTS') + $names | Set-Content -LiteralPath $def -Encoding ascii
$obsLib = Join-Path $buildRoot 'obs.lib'
& "$nativeTools\lib.exe" /nologo /machine:x64 "/def:$def" "/out:$obsLib"
if ($LASTEXITCODE) { throw 'Could not generate the OBS import library.' }
& cmake -S "$root\obs-plugin" -B "$buildRoot\obs" -G 'Visual Studio 17 2022' -A x64 "-DOBS_IMPORT_LIBRARY=$obsLib" "-DOBS_SDK=$DependencyRoot/obs-sdk" "-DSIMDE_INCLUDE=$DependencyRoot/simde" '-DOBS_JUKEBOX_QA=OFF'
if ($LASTEXITCODE) { throw 'OBS plugin configuration failed.' }
& cmake --build "$buildRoot\obs" --config $Configuration --parallel $Parallel
if ($LASTEXITCODE) { throw 'OBS plugin compilation failed.' }
$builtPlugin = "$buildRoot\obs\$Configuration\separate-song.dll"
$stagedPlugin = "$output\separate-song.dll"
if (!(Test-Path -LiteralPath $stagedPlugin) -or
    (Get-FileHash -LiteralPath $builtPlugin).Hash -ne (Get-FileHash -LiteralPath $stagedPlugin).Hash) {
    Copy-Item -LiteralPath $builtPlugin -Destination $stagedPlugin -Force
}
Copy-Item -LiteralPath "$DependencyRoot\fleym.nongd.geode" -Destination "$output\fleym.nongd.geode" -Force
Get-ChildItem -LiteralPath $output -File | Get-FileHash -Algorithm SHA256 | Format-Table Path,Hash -AutoSize
