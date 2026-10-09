"""Run deterministic Windows installer regressions from a clean checkout.

Requires Windows and .NET 9 SDK. Uses no release artifacts, downloaded mods,
local OBS/GD installation, or live application changes.
"""
import os
from pathlib import Path
import shutil
import subprocess
ROOT = Path(__file__).resolve().parents[2]
def main():
    if os.name != "nt":
        raise SystemExit("Installer tests require Windows (WinForms target).")
    dotnet = shutil.which("dotnet")
    if not dotnet:
        raise SystemExit("Install the .NET 9 SDK to run installer tests.")
    return subprocess.run([dotnet, "run", "--project", str(ROOT / "scripts/tests/installer/InstallerTests.csproj"), "--configuration", "Release"], cwd=ROOT, check=False).returncode
if __name__ == "__main__":
    raise SystemExit(main())
