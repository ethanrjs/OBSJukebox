"""bump-jukebox.py moves every Jukebox pin and the vendored headers to a new release."""
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OLD = re.search(r"set\(JUKEBOX_VERSION (\S+)\)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))[1]
SHA = "ab" * 32


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True).stdout


class BumpJukeboxTests(unittest.TestCase):
    def test_bumps_every_pin(self):
        with tempfile.TemporaryDirectory() as directory:
            repo, release = Path(directory, "repo"), Path(directory, "release")
            repo.mkdir()
            archive = git(ROOT, "archive", "HEAD")
            subprocess.run(["tar", "-x", "-C", str(repo)], input=archive, check=True)
            git(repo, "init", "-q")
            git(repo, "add", "-A")
            (release / "jukebox/jukebox/nong").mkdir(parents=True)
            (release / "jukebox/jukebox/nong/nong.hpp").write_text("// 9.9.9\n")
            (release / "jukebox/jukebox/nong/nong.cpp").write_text("// source\n")
            (release / "LICENSE").write_text("license\n")
            changelog = (repo / "changelog.md").read_bytes()
            link = (repo / "src/JukeboxLink.cpp").read_bytes()
            lock_path = repo / "scripts/dependencies.lock.json"
            other = json.loads(lock_path.read_text(encoding="utf-8"))["assets"]["cli-windows"]["url"]
            cli = re.search(r"/v([\d.]+)/", other)[1]
            lock_path.write_bytes(lock_path.read_bytes().replace(cli.encode(), OLD.encode()))
            other = other.replace(cli, OLD)

            subprocess.run([sys.executable, str(ROOT / "scripts/bump-jukebox.py"), "9.9.9", str(release), SHA,
                            "--root", str(repo)], check=True)

            git(repo, "add", "-A")
            for name in git(repo, "ls-files").decode().splitlines():
                if name.startswith(("vendor/", "src/", "release-notes/", "docs/")) or name in ("changelog.md", "scripts/dependencies.lock.json"):
                    continue
                data = (repo / name).read_bytes()
                for pin in (OLD, re.escape(OLD)):
                    self.assertNotIn(pin.encode(), data, name)
            self.assertIn(b"set(JUKEBOX_VERSION 9.9.9)", (repo / "CMakeLists.txt").read_bytes())
            lock = json.loads(lock_path.read_text(encoding="utf-8"))
            self.assertEqual(lock["assets"]["jukebox"]["sha256"], SHA)
            self.assertIn("/v9.9.9/", lock["assets"]["jukebox"]["url"])
            self.assertEqual(lock["assets"]["cli-windows"]["url"], other)
            self.assertIn(SHA.upper().encode(), (repo / "installer/windows/Program.cs").read_bytes())
            self.assertFalse((repo / f"vendor/jukebox-{OLD}").exists())
            self.assertEqual((repo / "vendor/jukebox-9.9.9/jukebox/nong/nong.hpp").read_text(), "// 9.9.9\n")
            self.assertTrue((repo / "vendor/jukebox-9.9.9/LICENSE").is_file())
            self.assertFalse((repo / "vendor/jukebox-9.9.9/jukebox/nong/nong.cpp").exists())
            self.assertEqual((repo / "changelog.md").read_bytes(), changelog)
            self.assertEqual((repo / "src/JukeboxLink.cpp").read_bytes(), link)


if __name__ == "__main__":
    unittest.main(verbosity=2)
