"""check-mod-package.py accepts this release's mod and refuses packages a loader could read differently."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[2]
EXPECTED = json.loads((ROOT / "mod.json").read_text(encoding="utf-8"))


def check(entries):
    with tempfile.TemporaryDirectory() as directory:
        package = Path(directory) / "mod.geode"
        with zipfile.ZipFile(package, "w") as archive:
            for name, data in entries:
                archive.writestr(name, data)
        return subprocess.run([sys.executable, str(ROOT / "scripts/check-mod-package.py"), str(package), "mod.dll"],
                              capture_output=True, text=True).returncode


MOD_JSON = json.dumps(dict(id=EXPECTED["id"], version=EXPECTED["version"]))


class CheckModPackageTests(unittest.TestCase):
    def test_accepts_this_release(self):
        self.assertEqual(check([("mod.json", MOD_JSON), ("mod.dll", "")]), 0)

    def test_refuses_a_missing_binary(self):
        self.assertNotEqual(check([("mod.json", MOD_JSON)]), 0)

    def test_refuses_a_second_mod_json(self):
        # zipfile reads the last of two entries with one name; a loader may read the first.
        other = json.dumps(dict(id="other.mod", version="v9.9.9"))
        with self.assertWarns(UserWarning):  # zipfile warns while writing the duplicate name
            self.assertNotEqual(check([("mod.json", other), ("mod.json", MOD_JSON), ("mod.dll", "")]), 0)

    def test_refuses_an_oversized_mod_json(self):
        padded = MOD_JSON[:-1] + ', "padding": "' + "x" * (1 << 20) + '"}'
        self.assertNotEqual(check([("mod.json", padded), ("mod.dll", "")]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
