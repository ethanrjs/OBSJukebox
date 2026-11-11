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

# CMake refuses a cache from another generator, and nested codegen/CPM caches survive --fresh, so delete the build directory.
function Reset-CMakeBuildDirectory {
    param([string]$BuildDirectory, [string]$Generator, [string]$RepositoryRoot)
    $cache = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (!(Test-Path -LiteralPath $cache)) { return }
    $cached = [regex]::Match((Get-Content -LiteralPath $cache -Raw), '(?m)^CMAKE_GENERATOR:INTERNAL=(.+)$').Groups[1].Value.Trim()
    if (!$cached -or $cached -eq $Generator) { return }
    $resolved = [IO.Path]::GetFullPath($BuildDirectory)
    $allowed = [IO.Path]::GetFullPath((Join-Path $RepositoryRoot 'build-windows')) + [IO.Path]::DirectorySeparatorChar
    if (!$resolved.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw "Refusing to remove ${resolved}: it is outside build-windows." }
    Write-Host "Removing ${resolved}: it was configured with '$cached'."
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
