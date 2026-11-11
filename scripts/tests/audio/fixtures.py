"""Generate the MP3 fixtures the audio regressions decode.

Both runners call this before compiling. The files are only regenerated when they are
missing or were produced by a different version of this script, so a CI cache can hold
them and ffmpeg is only needed on a cache miss.
"""
import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "artifacts/audio-tests"
STAMP = OUTPUT / ".fixtures-stamp"
FIXTURES = ("long.mp3", "short.mp3", "long-noxing.mp3", "long-44k-noxing.mp3")


def version():
    return hashlib.sha256(Path(__file__).read_bytes()).hexdigest()


def current():
    try:
        return STAMP.read_text().strip() == version() and all((OUTPUT / name).is_file() for name in FIXTURES)
    except OSError:
        return False


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)


def generate():
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is required to generate the MP3 regression fixtures")
    ffmpeg("-f", "lavfi", "-i", "sine=frequency=440:duration=240", "-ac", "2", "-ar", "48000", "-b:a", "64k", str(OUTPUT / "long.mp3"))
    ffmpeg("-i", str(OUTPUT / "long.mp3"), "-t", "4", "-c", "copy", str(OUTPUT / "short.mp3"))
    ffmpeg("-i", str(OUTPUT / "long.mp3"), "-c", "copy", "-write_xing", "0", str(OUTPUT / "long-noxing.mp3"))
    ffmpeg("-i", str(OUTPUT / "long.mp3"), "-ar", "44100", "-b:a", "64k", "-write_xing", "0", str(OUTPUT / "long-44k-noxing.mp3"))
    STAMP.write_text(version() + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="Regenerate even when the fixtures are current")
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    if args.force or not current():
        generate()
    else:
        print(f"MP3 fixtures are current in {OUTPUT}")
