import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--perl", default=shutil.which("perl"))
parser.add_argument("--loader", type=Path)
args = parser.parse_args()
if not args.perl:
    parser.error("Perl with its core JSON::PP module is required")


def binary(version="5.10.1", kind="pe", **metadata):
    if kind == "pe":
        header = bytearray(128)
        header[:2] = b"MZ"
        struct.pack_into("<I", header, 60, 64)
        header[64:68] = b"PE\0\0"
    else:
        header = bytes.fromhex("cffaedfe") + bytes(124)
    value = dict(id="geode.loader", version=version, geode=version)
    value.update(metadata)
    return bytes(header) + json.dumps(value).encode() + b"\0"


checks = 0


def check(ok, description):
    global checks
    assert ok, description
    checks += 1
    print("PASS", description)


with tempfile.TemporaryDirectory(prefix="obs-jukebox-geode-check-") as temporary:
    root = Path(temporary)
    fixture = root / "loader"

    def verify(data):
        fixture.write_bytes(data)
        return subprocess.run([args.perl, str(ROOT / "scripts/check-geode-version.pl"), str(fixture)], capture_output=True, text=True)

    for kind in ("pe", "mach"):
        for version in ("5.10.0", "5.10.1", "5.11.0", "5.100.1"):
            result = verify(binary(version, kind))
            check(result.returncode == 0 and result.stdout.strip() == version, f"{kind} accepts {version}")
        for version in ("5.9.9", "4.99.9", "6.10.1", "invalid", "5.10.1-beta.1", "5.010.1", "5.10"):
            check(verify(binary(version, kind)).returncode != 0, f"{kind} rejects {version}")
    check(verify(binary(geode="5.9.0")).returncode != 0, "rejects conflicting embedded version fields")
    check(verify(binary(id="other.dependency")).returncode != 0, "ignores dependency metadata")
    check(verify(binary() + binary("5.11.0")).returncode != 0, "rejects ambiguous embedded loader versions")
    check(verify(binary() + binary()).returncode == 0, "accepts repeated identical universal-binary metadata")
    check(verify(b"MZ" + bytes(126) + binary()[128:]).returncode != 0, "rejects invalid PE header")
    check(verify(b"MZ").returncode != 0, "rejects truncated library")
    check(verify(b'{"id":"geode.loader","version":"5.10.1","geode":"5.10.1"}\0').returncode != 0, "rejects metadata outside a library")
    check(verify(binary("5.9.0") + b'5.10.1\0{"version":"5.10.1","id":"dependency"}\0').returncode != 0, "does not accept unrelated version strings")
    if args.loader:
        result = verify(args.loader.read_bytes())
        check(result.returncode == 0, f"official loader {args.loader.name}: {result.stdout.strip()} {result.stderr.strip()}")

    if sys.platform == "linux" and os.geteuid() != 0:
        game = root / "game"
        prefix = root / "prefix"
        home = root / "home"
        commands = root / "commands"
        for directory in (game / "geode/mods", prefix / "drive_c", home, commands):
            directory.mkdir(parents=True)
        (game / "GeometryDash.exe").write_bytes(b"fixture, never executed")
        (game / "geode/mods/local.separate_song.geode").write_bytes(b"existing mod")
        (root / "plugin.so").write_bytes(b"fixture plugin, never executed")
        (root / "mod.geode").write_bytes(b"fixture mod")
        (commands / "pgrep").write_text("#!/bin/sh\nexit 1\n")
        (commands / "pgrep").chmod(0o755)
        env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(home / "config"), XDG_DATA_HOME=str(home / "data"), PATH=str(commands) + os.pathsep + os.environ["PATH"])
        command = ["bash", str(ROOT / "scripts/install-linux.sh"), "--game-dir", str(game), "--wine-prefix", str(prefix), "--plugin", str(root / "plugin.so"), "--mod", str(root / "mod.geode")]

        def snapshot():
            return {str(p.relative_to(root)): p.read_bytes() if p.is_file() else None for p in root.rglob("*")}

        for version in ("5.9.9", "4.99.9", "6.10.1", "invalid", "5.10.1-beta.1"):
            (game / "Geode.dll").write_bytes(binary(version))
            before = snapshot()
            result = subprocess.run(command, env=env, capture_output=True, text=True)
            check(result.returncode != 0 and "compatibility could not be verified" in result.stderr and snapshot() == before, f"Linux rejects {version} before any file mutations")
        (game / "Geode.dll").write_bytes(binary("5.10.0"))
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        check(result.returncode == 0 and (game / "geode/mods/local.separate_song.geode").read_bytes() == b"fixture mod", "Linux installs with patch-compatible 5.10.0")
    else:
        print("SKIP Linux installer integration: requires a non-root native Linux test process")
print(f"{checks} checks passed")
