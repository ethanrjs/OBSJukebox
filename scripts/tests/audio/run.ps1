param([switch]$BuildOnly)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
Push-Location $repo
try {
    New-Item -ItemType Directory -Force artifacts/audio-tests | Out-Null
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $vc = (Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1).FullName
    $sdk = "${env:ProgramFiles(x86)}/Windows Kits/10"
    $version = (Get-ChildItem "$sdk/Include" -Directory | Where-Object { Test-Path "$($_.FullName)/um/windows.h" } | Sort-Object Name -Descending | Select-Object -First 1).Name
    if (!$vc -or !$version) { throw 'MSVC and a Windows SDK are required.' }
    $includes = @("/I$vc/include", "/I$sdk/Include/$version/ucrt", "/I$sdk/Include/$version/um", "/I$sdk/Include/$version/shared", "/I$sdk/Include/$version/winrt")
    & "$vc/bin/Hostx64/x64/cl.exe" /nologo /std:c++20 /O2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Iscripts/tests/audio/stubs @includes scripts/tests/audio/playback-tests.cpp /Foartifacts/audio-tests/playback-tests.obj /Feartifacts/audio-tests/playback-tests.exe /link "/LIBPATH:$vc/lib/x64" "/LIBPATH:$sdk/Lib/$version/ucrt/x64" "/LIBPATH:$sdk/Lib/$version/um/x64" ws2_32.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib
    if ($LASTEXITCODE) { throw 'Audio test compilation failed.' }
    if (!$BuildOnly) {
        & ./artifacts/audio-tests/playback-tests.exe
        if ($LASTEXITCODE) { throw 'Audio regression tests failed.' }
    }
} finally { Pop-Location }
