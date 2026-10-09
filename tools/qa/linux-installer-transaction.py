import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile
import json


ROOT = Path(__file__).resolve().parents[2]


class InstallerTransactionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="obs-jukebox-transaction-")
        self.root = Path(self.temp.name)
        self.game = self.root / "game"
        self.prefix = self.root / "prefix"
        self.config = self.root / "config"
        self.data = self.root / "data"
        self.package = self.root / "package"
        self.bin = self.root / "bin"
        for path in (self.game / "geode/mods", self.prefix / "drive_c", self.package / "payload", self.bin):
            path.mkdir(parents=True)
        (self.game / "GeometryDash.exe").touch()
        (self.game / "Geode.dll").touch()
        shutil.copyfile(ROOT / "scripts/install-linux.sh", self.package / "Install.sh")
        shutil.copyfile(ROOT / "scripts/install-state.pl", self.package / "install-state.pl")
        (self.package / "check-geode-version.pl").write_text('print "5.10.1\\n";')
        (self.package / "payload/separate-song.so").write_bytes(b"NEWPLUGIN")
        (self.package / "payload/local.separate_song.geode").write_bytes(b"NEWMOD")
        self.targets = [
            self.config / "obs-studio/plugins/separate-song/bin/64bit/separate-song.so",
            self.game / "geode/mods/local.separate_song.geode",
            self.config / "obs-jukebox/paths",
            self.data / "flatpak/overrides/com.obsproject.Studio",
        ]
        self.originals = [b"OLDPLUGIN", b"OLDMOD", b"OLDPATHS", b"[Context]\nfilesystems=xdg-pictures:ro;\n"]
        for path, data in zip(self.targets, self.originals):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            path.chmod(0o640)
        for name, mod_id in (("local.separate_song", "local.separate_song"), ("fleym.nongd", "fleym.nongd")):
            with zipfile.ZipFile(self.game / f"geode/mods/{name}.geode", "w") as mod:
                mod.writestr("mod.json", json.dumps(dict(id=mod_id, version="3.8.0" if name == "fleym.nongd" else "1.1.0")))
                mod.writestr(f"{mod_id}.dll", "fixture")
            (self.game / f"geode/mods/{name}.geode").chmod(0o640)
        self.originals[1] = self.targets[1].read_bytes()
        self.wrapper("uname", 'if [[ "$1" == -s ]]; then echo Linux; else echo x86_64; fi')
        self.wrapper("pgrep", "exit 1")
        self.wrapper("install", 'if [[ "$FAIL_AT" == stage-mod && "$*" == *payload/local.separate_song.geode* ]]; then exit 71; fi\nexec /usr/bin/install "$@"')
        self.wrapper("mv", 'target="${!#}"\nsource="${@: -2:1}"\nif [[ "$source" == *obs-jukebox-new* ]] && { [[ "$FAIL_AT" == commit-mod && "$target" == */local.separate_song.geode ]] || [[ "$FAIL_AT" == commit-paths && "$target" == */paths ]]; }; then exit 72; fi\nexec /usr/bin/mv "$@"')
        self.wrapper("flatpak", '[[ "$1" != info ]] || exit 0\nprintf "[Context]\\nfilesystems=host;\\n" > "$XDG_DATA_HOME/flatpak/overrides/com.obsproject.Studio"\n[[ "$FAIL_AT" != flatpak ]]')
        self.env = dict(os.environ, HOME=str(self.root / "home"), XDG_CONFIG_HOME=str(self.config), XDG_DATA_HOME=str(self.data), PATH=str(self.bin) + os.pathsep + os.environ["PATH"], FAIL_AT="")

    def tearDown(self):
        self.temp.cleanup()

    def wrapper(self, name, content):
        path = self.bin / name
        path.write_text("#!/usr/bin/env bash\n" + content + "\n")
        path.chmod(0o755)

    def run_installer(self, failure="", flatpak=False):
        if flatpak:
            flat_config = Path(self.env["HOME"]) / ".var/app/com.obsproject.Studio/config"
            for index in (0, 2):
                previous = self.targets[index]
                target = flat_config / previous.relative_to(self.config)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(previous, target)
                self.targets[index] = target
        self.env["FAIL_AT"] = failure
        args = ["bash", str(self.package / "Install.sh"), "--game-dir", str(self.game), "--wine-prefix", str(self.prefix)]
        if flatpak:
            args.append("--flatpak")
        result = subprocess.run(args, env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, not bool(failure), result.stdout + result.stderr)
        self.assertFalse(list(self.root.rglob(".obs-jukebox-*")))
        return result

    def assert_originals(self, missing=()):
        for index, (path, data) in enumerate(zip(self.targets, self.originals)):
            if index in missing:
                self.assertFalse(path.exists())
            else:
                self.assertEqual(path.read_bytes(), data)
                self.assertEqual(path.stat().st_mode & 0o777, 0o640)

    def undo(self):
        result = subprocess.run(["bash", str(self.package / "Install.sh"), "--uninstall"], env=self.env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_undo_restores_previous_install(self):
        self.run_installer()
        self.undo()
        self.assert_originals()

    def test_undo_preserves_user_modified_mod(self):
        self.run_installer()
        self.targets[1].write_bytes(b"USERMOD")
        self.undo()
        self.assertEqual(self.targets[1].read_bytes(), b"USERMOD")
        self.assertEqual(self.targets[0].read_bytes(), self.originals[0])

    def test_missing_jukebox_rejected(self):
        (self.game / "geode/mods/fleym.nongd.geode").unlink()
        self.run_installer("missing-jukebox")
        self.assert_originals()

    def test_staging_failure_preserves_all_files(self):
        self.run_installer("stage-mod")
        self.assert_originals()

    def test_unwritable_mod_directory_preserves_all_files(self):
        directory = self.targets[1].parent
        directory.chmod(0o555)
        try:
            self.run_installer("unwritable-mod")
            self.assert_originals()
        finally:
            directory.chmod(0o755)

    def test_mod_commit_failure_restores_plugin(self):
        self.run_installer("commit-mod")
        self.assert_originals()

    def test_paths_commit_failure_restores_both_binaries(self):
        self.run_installer("commit-paths")
        self.assert_originals()

    def test_flatpak_failure_restores_files_and_permissions(self):
        self.run_installer("flatpak", flatpak=True)
        self.assert_originals()

    def test_flatpak_failure_removes_new_override(self):
        self.targets[3].unlink()
        self.run_installer("flatpak", flatpak=True)
        self.assert_originals(missing=(3,))

    def test_fresh_install_failure_removes_partial_files(self):
        for path in self.targets:
            path.unlink()
        self.run_installer("commit-paths")
        self.assert_originals(missing=(0, 1, 2, 3))

    def test_success_replaces_all_files_and_keeps_backups(self):
        self.run_installer()
        self.assertEqual(self.targets[0].read_bytes(), b"NEWPLUGIN")
        self.assertEqual(self.targets[1].read_bytes(), b"NEWMOD")
        self.assertEqual(self.targets[2].read_text(), str(self.prefix) + "\n" + str(self.game) + "\n")
        backups = list((self.data / "obs-jukebox/backups").iterdir())
        self.assertEqual(len(backups), 1)
        for path, data in zip(self.targets[:3], self.originals):
            self.assertEqual((backups[0] / path.name).read_bytes(), data)


if __name__ == "__main__":
    unittest.main(verbosity=2)
