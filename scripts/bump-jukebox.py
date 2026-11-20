"""Move every Jukebox version and hash pin, and the vendored headers, to a new Jukebox release."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess

UNCHANGED = ("vendor/", "src/", "release-notes/", "docs/", "changelog.md")
LOCK = "scripts/dependencies.lock.json"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("version", help="new Jukebox version, without the v")
    parser.add_argument("release", type=Path, help="checkout of the release tag")
    parser.add_argument("sha256", help="SHA-256 of the release's fleym.nongd.geode")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root, new = args.root, args.version
    old = re.search(r"set\(JUKEBOX_VERSION (\S+)\)", (root / "CMakeLists.txt").read_text(encoding="utf-8"))[1]
    jukebox = json.loads((root / LOCK).read_text(encoding="utf-8"))["assets"]["jukebox"]
    old_sha = jukebox["sha256"]
    pins = [(old, new), (re.escape(old), re.escape(new)), (old_sha.lower(), args.sha256.lower()),
            (old_sha.upper(), args.sha256.upper())]
    lock_pins = [(jukebox["url"], jukebox["url"].replace(old, new)), (old_sha, args.sha256.lower())]

    names = subprocess.run(["git", "-C", str(root), "ls-files"], check=True, capture_output=True, text=True)
    for name in names.stdout.splitlines():
        path = root / name
        if name.startswith(UNCHANGED) or not path.is_file():
            continue
        data = path.read_bytes()
        changed = data
        for before, after in lock_pins if name == LOCK else pins:
            changed = changed.replace(before.encode(), after.encode())
        if changed != data:
            path.write_bytes(changed)
            print(f"updated {name}")

    shutil.rmtree(root / f"vendor/jukebox-{old}")
    vendored = root / f"vendor/jukebox-{new}"
    shutil.copytree(args.release / "jukebox/jukebox", vendored / "jukebox", ignore=shutil.ignore_patterns("*.cpp"))
    shutil.copy2(args.release / "LICENSE", vendored / "LICENSE")
    print(f"vendored {args.release} as {vendored.relative_to(root).as_posix()}")


if __name__ == "__main__":
    main()
