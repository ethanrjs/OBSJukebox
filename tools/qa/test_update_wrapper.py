import json
import os
import pathlib
import subprocess
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WrapperTests(unittest.TestCase):
    def test_terminal_outcomes_and_obs_wait(self):
        for code in (0, 1, 3, 4, 137):
            with self.subTest(code=code), tempfile.TemporaryDirectory() as directory:
                folder = pathlib.Path(directory)
                (folder / "payload").mkdir()
                with zipfile.ZipFile(folder / "payload/local.separate_song.geode", "w") as archive:
                    archive.writestr("mod.json", '{"version":"1.2.5"}')
                flag = folder / "reopen"
                flag.touch()
                events = folder / "events"

                def executable(name, body):
                    path = folder / name
                    path.write_text("#!/bin/sh\n" + body + "\n")
                    path.chmod(0o700)
                    return str(path)

                uname = executable("uname", "echo Darwin")
                plist = executable("plist", "echo OBS")
                mktemp = executable("mktemp", 'exec /usr/bin/mktemp "$HOME/tmp.XXXXXX"')
                opened = executable("open", 'echo open >> "$HOME/events"')
                                                                                                
                pgrep = executable("pgrep", '''[ "$2" = OBS ] || exit 1
n=$(cat "$HOME/polls" 2>/dev/null || echo 0)
n=$((n+1)); echo "$n" > "$HOME/polls"
[ "$n" -le 2 ]''')
                sleep = executable("sleep", 'echo wait >> "$HOME/events"')
                child = executable("child", f'echo install >> "$HOME/events"\nexit {code}')
                source = (ROOT / "scripts/Install.command").read_text()
                for before, after in (("/usr/bin/uname", uname), ("/usr/libexec/PlistBuddy", plist),
                                      ("/usr/bin/mktemp", mktemp), ("/usr/bin/open", opened),
                                      ("/usr/bin/pgrep", pgrep), ("sleep 2", sleep)):
                    source = source.replace(before, after)
                source = source.replace('/bin/zsh "$song_root/Install.command" --no-downgrade', child)
                script = folder / "Install.command"
                script.write_text(source)
                result = folder / "result.json"
                subprocess.run(["zsh", str(script), "--update", "--result", str(result),
                                "--reopen-flag", str(flag), str(folder / "GD.app"), str(folder / "OBS.app")],
                               env={**os.environ, "HOME": directory}, check=True, capture_output=True, timeout=10)
                self.assertEqual(events.read_text().splitlines(), ["wait", "wait", "install", "open"])
                self.assertFalse(flag.exists())
                report = json.loads(result.read_text())
                self.assertEqual(report["status"], "installed" if code == 0 else "waiting" if code == 4 else "failed")
                self.assertEqual(report.get("repair", False), code in (3, 137))


if __name__ == "__main__":
    unittest.main()
