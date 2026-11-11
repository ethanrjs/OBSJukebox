param([switch]$BuildOnly, [switch]$Bench, [Parameter(ValueFromRemainingArguments)][string[]]$Arguments)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
# -Bench builds bench.cpp instead of the tests; other arguments go to the program.
$program = if ($Bench) { 'bench' } else { 'playback-tests' }
Push-Location $repo
try {
    & python scripts/tests/audio/fixtures.py
    if ($LASTEXITCODE) { throw 'MP3 fixture generation failed.' }
    . ./scripts/windows-env.ps1
    Enter-MsvcEnvironment | Out-Null
    & cl.exe /nologo /DOBS_JUKEBOX_QA /std:c++20 /O2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Iscripts/tests/audio/stubs "scripts/tests/audio/$program.cpp" obs-plugin/Decoder.cpp obs-plugin/PitchSimd.cpp /Foartifacts/audio-tests/ "/Feartifacts/audio-tests/$program.exe" /link ws2_32.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib winmm.lib avrt.lib
    if ($LASTEXITCODE) { throw 'Audio test compilation failed.' }
    if (!$BuildOnly) {
        & "./artifacts/audio-tests/$program.exe" @Arguments
        if ($LASTEXITCODE) { throw 'Audio regression tests failed.' }
    }
} finally { Pop-Location }
