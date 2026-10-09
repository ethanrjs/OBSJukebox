param(
    [string]$ObsDirectory = "$env:ProgramFiles\obs-studio",
    [string]$PluginPath,
    [string]$OutputDirectory,
    [string]$Mp3Path,
    [ValidateRange(1024,65535)][int]$Port = 49178
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (!$PluginPath) {
    $qaPluginBuild = Join-Path $root 'build-windows\obs-runtime-qa'
    & cmake -S "$root\obs-plugin" -B $qaPluginBuild -G 'Visual Studio 17 2022' -A x64 "-DOBS_IMPORT_LIBRARY=$root/build-windows/obs.lib" '-DOBS_JUKEBOX_QA=ON'
    if ($LASTEXITCODE) { throw 'QA plugin configuration failed. Build the Windows plugin once to generate its OBS import library.' }
    & cmake --build $qaPluginBuild --config Release --parallel 2
    if ($LASTEXITCODE) { throw 'QA plugin build failed.' }
    $PluginPath = Join-Path $qaPluginBuild 'Release\separate-song.dll'
}
if (!$OutputDirectory) { $OutputDirectory = Join-Path $root 'artifacts\windows-validation' }
$obsBin = Join-Path $ObsDirectory 'bin\64bit'
$endpoint = Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue
if ($endpoint) { throw "UDP $Port is in use. Choose an unused test port." }
if ($Port -eq 39022 -and (Get-Process -Name GeometryDash -ErrorAction SilentlyContinue)) { throw 'Stop Geometry Dash before synthetic validation; its real link packets would interfere with these cases.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

& cmake -S "$PSScriptRoot" -B "$root\build-windows\qa" -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw 'QA configure failed.' }
& cmake --build "$root\build-windows\qa" --config Release --parallel 2
if ($LASTEXITCODE) { throw 'QA build failed.' }
& ctest --test-dir "$root\build-windows\qa" -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Game identity regression failed.' }
if (!$Mp3Path) {
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($ffmpeg) {
        $Mp3Path = Join-Path $OutputDirectory 'runtime-tone.mp3'
        & $ffmpeg.Source -hide_banner -loglevel error -f lavfi -i 'sine=frequency=440:duration=6:sample_rate=48000' -ac 2 -codec:a libmp3lame -y $Mp3Path
        if ($LASTEXITCODE) { throw 'MP3 fixture generation failed.' }
    }
}
$savedPath = $env:PATH
$savedTestPort = $env:OBS_JUKEBOX_TEST_PORT
$savedObsDirectory = $env:OBS_QA_DIRECTORY
try {
    $env:OBS_JUKEBOX_TEST_PORT = "$Port"
    $env:OBS_QA_DIRECTORY = (Resolve-Path -LiteralPath $ObsDirectory).Path
    $env:PATH = "$obsBin;$savedPath"
    $arguments = @((Resolve-Path -LiteralPath $PluginPath).Path,(Resolve-Path -LiteralPath $OutputDirectory).Path)
    if ($Mp3Path) { $arguments += (Resolve-Path -LiteralPath $Mp3Path).Path }
    & "$root\build-windows\qa\Release\obs-runtime-validation.exe" @arguments *> (Join-Path $OutputDirectory 'runtime-log.txt')
    $harnessExit = $LASTEXITCODE
} finally { $env:PATH = $savedPath; $env:OBS_JUKEBOX_TEST_PORT = $savedTestPort; $env:OBS_QA_DIRECTORY = $savedObsDirectory }
if (!(Test-Path -LiteralPath (Join-Path $OutputDirectory 'runtime-results.json'))) {
    throw "Runtime harness exited $harnessExit before producing results. Inspect $OutputDirectory\runtime-log.txt"
}
$results = Get-Content -LiteralPath (Join-Path $OutputDirectory 'runtime-results.json') -Raw | ConvertFrom-Json
$results.cases | Format-Table name,frames,raw_rms,frequency_hz,pass -AutoSize
$evidence = [ordered]@{
    validatedAt = [DateTimeOffset]::Now.ToString('o')
    harnessExitCode = $harnessExit
    artifacts = @(@($PluginPath,(Join-Path $obsBin 'obs.dll'),"$PSScriptRoot\obs-runtime-validation.cpp",(Join-Path $OutputDirectory 'runtime-results.json')) | ForEach-Object {
        $hash = Get-FileHash -LiteralPath $_ -Algorithm SHA256
        [ordered]@{ path=$hash.Path; sha256=$hash.Hash }
    })
}
$evidence | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'runtime-manifest.json') -Encoding utf8
if ($harnessExit -or $results.failures) { throw "Runtime validation failed. Inspect $OutputDirectory\runtime-log.txt" }


$decoderFixtures = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter 'runtime-*.wav' | ForEach-Object FullName)
if ($Mp3Path) {
    $decoderFixtures += $Mp3Path
    $ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
    if ($ffmpeg) {
        foreach ($extension in @('ogg','flac')) {
            $fixture = Join-Path $OutputDirectory "decoder-tone.$extension"
            & $ffmpeg.Source -hide_banner -loglevel error -i $Mp3Path -y $fixture
            if ($LASTEXITCODE) { throw "Could not generate $extension decoder fixture." }
            $decoderFixtures += $fixture
        }
    }
}
& "$root\build-windows\qa\Release\decoder-seek-validation.exe" @decoderFixtures | Tee-Object -FilePath (Join-Path $OutputDirectory 'decoder-seek-results.txt')
if ($LASTEXITCODE) { throw 'Decoder seek regression failed.' }

$formatTest = "$root\build-windows\qa\Release\decoder-format-validation.exe"
$formatWave = Join-Path $OutputDirectory 'format-tone.wav'
& $formatTest --fixture $formatWave
if ($LASTEXITCODE) { throw 'Decoder format fixture generation failed.' }
$formatFixtures = @($formatWave)
$ffmpeg = Get-Command ffmpeg -ErrorAction SilentlyContinue
if ($ffmpeg) {
    foreach ($extension in @('mp3','ogg','flac','aac','m4a','aiff')) {
        $fixture = Join-Path $OutputDirectory "format-tone.$extension"
        & $ffmpeg.Source -hide_banner -loglevel error -i $formatWave -y $fixture
        if ($LASTEXITCODE) { throw "Could not generate $extension format fixture." }
        $formatFixtures += $fixture
    }
} else {
    Write-Warning 'ffmpeg is unavailable; decoder format checks cover WAV only.'
}
& $formatTest @formatFixtures | Tee-Object -FilePath (Join-Path $OutputDirectory 'decoder-format-results.txt')
if ($LASTEXITCODE) { throw 'Decoder format regression failed.' }
