param([switch]$BuildOnly, [switch]$Bench, [Parameter(ValueFromRemainingArguments)][string[]]$Arguments)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
# -Bench builds bench.cpp instead of the tests; other arguments go to the program.
$program = if ($Bench) { 'bench' } else { 'playback-tests' }
Push-Location $repo
try {
    New-Item -ItemType Directory -Force artifacts/audio-tests | Out-Null
    if (!(Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw 'ffmpeg is required for the MP3 regression fixture.' }
    & ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'sine=frequency=440:duration=240' -ac 2 -ar 48000 -b:a 64k artifacts/audio-tests/long.mp3
    if ($LASTEXITCODE) { throw 'MP3 fixture generation failed.' }
    & ffmpeg -hide_banner -loglevel error -y -i artifacts/audio-tests/long.mp3 -t 4 -c copy artifacts/audio-tests/short.mp3
    if ($LASTEXITCODE) { throw 'Short MP3 fixture generation failed.' }
    & ffmpeg -hide_banner -loglevel error -y -i artifacts/audio-tests/long.mp3 -c copy -write_xing 0 artifacts/audio-tests/long-noxing.mp3
    if ($LASTEXITCODE) { throw 'MP3 fixture without a Xing header failed.' }
    & ffmpeg -hide_banner -loglevel error -y -i artifacts/audio-tests/long.mp3 -ar 44100 -b:a 64k -write_xing 0 artifacts/audio-tests/long-44k-noxing.mp3
    if ($LASTEXITCODE) { throw '44.1 kHz MP3 fixture without a Xing header failed.' }
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vc = (Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    $sdk = "${env:ProgramFiles(x86)}/Windows Kits/10"
    $version = (Get-ChildItem "$sdk/Include" -Directory | Where-Object { Test-Path "$($_.FullName)/um/windows.h" } | Sort-Object Name -Descending | Select-Object -First 1).Name
    if (!$vc -or !$version) { throw 'MSVC and a Windows SDK are required.' }
    $includes = @("/I$vc/include", "/I$sdk/Include/$version/ucrt", "/I$sdk/Include/$version/um", "/I$sdk/Include/$version/shared", "/I$sdk/Include/$version/winrt")
    & "$vc/bin/Hostx64/x64/cl.exe" /nologo /DOBS_JUKEBOX_QA /std:c++20 /O2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Iscripts/tests/audio/stubs @includes "scripts/tests/audio/$program.cpp" obs-plugin/Decoder.cpp obs-plugin/PitchSimd.cpp /Foartifacts/audio-tests/ "/Feartifacts/audio-tests/$program.exe" /link "/LIBPATH:$vc/lib/x64" "/LIBPATH:$sdk/Lib/$version/ucrt/x64" "/LIBPATH:$sdk/Lib/$version/um/x64" ws2_32.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib winmm.lib avrt.lib
    if ($LASTEXITCODE) { throw 'Audio test compilation failed.' }
    if (!$BuildOnly) {
        & "./artifacts/audio-tests/$program.exe" @Arguments
        if ($LASTEXITCODE) { throw 'Audio regression tests failed.' }
    }
} finally { Pop-Location }
