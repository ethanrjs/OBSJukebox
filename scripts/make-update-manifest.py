#!/usr/bin/env python3
"""Writes update.json for a release: its version and the size and SHA-256 of each setup.

The output is canonical (sorted keys, no spaces, one trailing newline), so the same files always
give the same bytes, and the mod accepts only that exact form.
"""
import argparse
import hashlib
import json
import pathlib
import re

VERSION = r"(\d{1,9}\.\d{1,9}\.\d{1,9})"
NAMES = {
    "windows": re.compile(rf"OBS-Jukebox-{VERSION}-Windows-Setup\.exe"),
    "macos-arm64": re.compile(rf"OBS-Jukebox-{VERSION}-macOS-arm64\.zip"),
}


def manifest(windows: pathlib.Path, mac: pathlib.Path, mod_version: str, tag: str) -> bytes:
    files, versions = {}, set()
    for key, path in (("windows", windows), ("macos-arm64", mac)):
        match = NAMES[key].fullmatch(path.name)
        if not match:
            raise SystemExit(f"Unexpected file name: {path.name}")
        versions.add(match[1])
        data = path.read_bytes()
        files[key] = {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
    everything = versions | {mod_version.removeprefix("v"), tag.removeprefix("v")}
    if len(everything) != 1:
        raise SystemExit(f"Versions disagree: files {sorted(versions)}, mod.json {mod_version}, tag {tag}")
    body = {"files": files, "schema": 1, "version": everything.pop()}
    return (json.dumps(body, sort_keys=True, separators=(",", ":")) + "\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--mod-json", type=pathlib.Path, default=pathlib.Path("mod.json"))
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("windows", type=pathlib.Path)
    parser.add_argument("mac", type=pathlib.Path)
    args = parser.parse_args()
    mod_version = json.loads(args.mod_json.read_text(encoding="utf-8"))["version"]
    args.out.write_bytes(manifest(args.windows, args.mac, mod_version, args.tag))


if __name__ == "__main__":
    main()
