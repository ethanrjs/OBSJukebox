# Dot-source this file for the Windows build environment helpers.

# Puts cl.exe, the Windows SDK and the linker tools on PATH for the current session.
function Enter-MsvcEnvironment {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio 2022 C++ Build Tools are required.' }
    $env:PATH = "$(Split-Path $vswhere);$env:PATH"
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw 'The MSVC developer environment did not provide cl.exe.' }
    return $vs
}

# CMake refuses a cache from another generator, loses settings when the compiler changes, and nested
# codegen/CPM caches survive --fresh, so delete the build directory.
function Reset-CMakeBuildDirectory {
    param([string]$BuildDirectory, [string]$Generator, [string]$RepositoryRoot, [string]$Compiler)
    $cache = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cache)) { return }
    $text = Get-Content -LiteralPath $cache -Raw -Encoding UTF8
    $cachedGenerator = [regex]::Match($text, '(?m)^CMAKE_GENERATOR:INTERNAL=(.+)$').Groups[1].Value.Trim()
    $cachedCompiler = [regex]::Match($text, '(?m)^CMAKE_CXX_COMPILER:\w+=(.+)$').Groups[1].Value.Trim()
    $sameGenerator = !$cachedGenerator -or $cachedGenerator -eq $Generator
    $sameCompiler = !$Compiler -or !$cachedCompiler -or $cachedCompiler -eq $Compiler
    if ($sameGenerator -and $sameCompiler) { return }
    $resolved = [IO.Path]::GetFullPath($BuildDirectory)
    $allowed = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'build-windows')) + [IO.Path]::DirectorySeparatorChar
    if (!$resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to remove ${resolved}: it is outside build-windows." }
    Write-Host "Removing ${resolved}: it was configured with '$cachedGenerator' and '$cachedCompiler'."
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
