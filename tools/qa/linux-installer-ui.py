#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import tkinter as tk

source = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("linux_setup", source / "installer/linux/installer.py")
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)
checks = 0


def check(value, message):
    global checks
    assert value, message
    checks += 1
    print("PASS:", message)


def finish(root, app):
    deadline = time.monotonic() + 10
    while app.busy and time.monotonic() < deadline:
        root.update()
        time.sleep(.02)
    root.update()
    check(not app.busy, "installer returns to idle")


with tempfile.TemporaryDirectory(prefix="jukebox-gui-") as folder:
    fixture = Path(folder)
    os.environ["HOME"] = str(fixture / "home")
    os.environ["XDG_DATA_HOME"] = str(fixture / "state")
    os.environ.pop("XDG_CONFIG_HOME", None)
    package = fixture / "package"
    package.mkdir()
    shutil.copy(source / "installer/windows/logo.png", package / "logo.png")
    game = fixture / "Geometry Dash ; $literal"
    game.mkdir()
    (game / "GeometryDash.exe").touch()
    prefix = fixture / "Proton prefix"
    (prefix / "drive_c").mkdir(parents=True)
    backend = package / "Install.sh"
    backend.write_text('#!/bin/bash\nprintf "%s\\n" "$@" > "$(dirname "$0")/calls.txt"\nprintf "Installed fixture.\\n"\n')
    launcher = package / "OBS-Jukebox-Setup"
    shutil.copy(source / "installer/linux/OBS-Jukebox-Setup", launcher)
    subprocess.run(["bash", "-n", str(launcher)], check=True)
    launched = subprocess.run(["bash", str(launcher), "--cli", "--game-dir", str(game)], capture_output=True, text=True)
    check(launched.returncode == 0 and (package / "calls.txt").read_text().splitlines() == ["--game-dir", str(game)], "launcher CLI fallback preserves arguments")
    (package / "calls.txt").unlink()
    rejected = subprocess.run(["bash", str(launcher), "--unknown"], capture_output=True, text=True)
    check(rejected.returncode == 2 and "Usage:" in rejected.stderr, "launcher rejects unsupported options")
    check(setup.install_command(package, str(game), "", False) == ["bash", str(backend), "--game-dir", str(game)], "paths including shell metacharacters remain single arguments")
    try:
        setup.install_command(package, str(game), str(fixture / "missing"), False)
        raise AssertionError("missing prefix accepted")
    except ValueError:
        check(True, "invalid Proton prefix rejected")
    root = tk.Tk()
    app = setup.SetupWindow(root, package)
    root.update()
    check(root.winfo_width() == 730 and root.winfo_height() == 650, "window matches Windows setup dimensions")
    check(not app.reopen.get(), "reopen requires explicit selection")
    app.install_button.invoke()
    check("GeometryDash.exe" in app.status.get() and not app.busy, "missing game rejected before backend launch")
    check(not (package / "calls.txt").exists(), "invalid input made no backend calls")
    app.game.set(str(game))
    app.prefix.set(str(prefix))
    app.obs.set("Flatpak OBS Studio")
    app.install_button.invoke()
    check(app.busy and str(app.install_button["state"]) == "disabled", "installation disables inputs")
    finish(root, app)
    check((package / "calls.txt").read_text().splitlines() == ["--game-dir", str(game), "--wine-prefix", str(prefix), "--flatpak"], "Flatpak and custom prefix reach bash backend")
    check(app.status.get().startswith("Installed."), "success visible in status")
    check(len(list((setup.data_root() / "SetupLogs").glob("*.log"))) == 1, "successful backend log preserved")
    backend.write_text('#!/bin/bash\nprintf "Geode compatibility could not be verified. No files changed.\\n"\nexit 1\n')
    app.install_button.invoke()
    finish(root, app)
    check("Geode compatibility" in app.status.get(), "backend failure shown instead of success")
    check(len(list((setup.data_root() / "SetupLogs").glob("*.log"))) == 2, "failure log preserved separately")
    check(str(app.inputs[2]["state"]) == "readonly", "OBS selection restored as readonly")
    if len(sys.argv) > 1:
        from PIL import ImageGrab
        app.game.set("/home/ethan/.local/share/Steam/steamapps/common/Geometry Dash")
        app.prefix.set("")
        app.obs.set("Native OBS Studio")
        app.status.set("Finish any OBS recording and close GD and OBS, then click Install.")
        root.geometry("730x650+0+0")
        root.update()
        ImageGrab.grab(bbox=(root.winfo_rootx(), root.winfo_rooty(), root.winfo_rootx() + 730, root.winfo_rooty() + 650)).save(sys.argv[1])
    root.destroy()
print(f"{checks}/{checks} Linux installer UI checks passed.")
