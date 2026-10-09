#!/usr/bin/env python3

import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


def data_root():
    return Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share"))) / "obs-jukebox"


def find_game():
    for steam in (".local/share/Steam", ".steam/steam", ".var/app/com.valvesoftware.Steam/.local/share/Steam"):
        game = Path.home() / steam / "steamapps/common/Geometry Dash"
        if (game / "GeometryDash.exe").is_file():
            return str(game)
    return ""


def install_command(package, game, prefix, flatpak):
    game = game.strip()
    prefix = prefix.strip()
    if not game or not (Path(game) / "GeometryDash.exe").is_file():
        raise ValueError("Choose the Geometry Dash folder containing GeometryDash.exe.")
    if "\n" in game or "\r" in game or "\n" in prefix or "\r" in prefix:
        raise ValueError("Installation paths cannot contain line breaks.")
    if prefix and not (Path(prefix) / "drive_c").is_dir():
        raise ValueError("Choose the Proton prefix folder containing drive_c, or leave it blank for automatic detection.")
    backend = package / "Install.sh"
    if not backend.is_file():
        raise ValueError("Install.sh is missing. Extract the complete OBS Jukebox package and try again.")
    command = ["bash", str(backend), "--game-dir", str(Path(game).resolve())]
    if prefix:
        command += ["--wine-prefix", str(Path(prefix).resolve())]
    if flatpak:
        command += ["--flatpak"]
    return command


class SetupWindow:
    def __init__(self, root, package):
        self.root, self.package = root, Path(package)
        self.events = queue.Queue()
        self.busy = False
        self.inputs = []
        self.game = tk.StringVar(value=find_game())
        self.prefix = tk.StringVar()
        self.obs = tk.StringVar(value="Native OBS Studio")
        self.reopen = tk.BooleanVar(value=False)
        self.status = tk.StringVar(value="Finish any OBS recording and close GD and OBS, then click Install.")
        root.title("OBS Jukebox - Linux Setup")
        root.geometry("730x650")
        root.minsize(730, 650)
        style = ttk.Style(root)
        style.theme_use("clam")
        style.configure(".", font=("DejaVu Sans", 10))
        style.configure("TFrame", background="#f0f0f0")
        style.configure("TLabel", background="#f0f0f0")
        style.configure("TCheckbutton", background="#f0f0f0")
        style.configure("Title.TLabel", font=("DejaVu Sans", 23, "bold"))
        style.configure("Status.TLabel", font=("DejaVu Sans", 11, "bold"))
        outer = ttk.Frame(root, padding=22)
        outer.grid(sticky="nsew")
        root.columnconfigure(0, weight=1)
        root.rowconfigure(0, weight=1)
        outer.columnconfigure(0, weight=1)
        heading = ttk.Frame(outer)
        heading.grid(row=0, column=0, sticky="w")
        logo = self.package / "logo.png"
        if logo.is_file():
            full = tk.PhotoImage(file=str(logo))
            self.logo = full.subsample(max(1, round(full.width() / 42)))
            ttk.Label(heading, image=self.logo).pack(side="left", padx=(0, 12))
            root.iconphoto(True, self.logo)
        ttk.Label(heading, text="OBS Jukebox", style="Title.TLabel").pack(side="left")
        ttk.Label(outer, text="Separate GD/Song output for OBS").grid(row=1, column=0, sticky="w", pady=(4, 12))
        self.path_row(outer, 2, "Geometry Dash folder", self.game)
        obs_row = ttk.Frame(outer)
        obs_row.grid(row=3, column=0, sticky="ew", pady=6)
        obs_row.columnconfigure(0, weight=1)
        ttk.Label(obs_row, text="OBS Studio installation").grid(row=0, column=0, sticky="w")
        selection = ttk.Combobox(obs_row, textvariable=self.obs, values=("Native OBS Studio", "Flatpak OBS Studio"), state="readonly")
        selection.grid(row=1, column=0, sticky="ew", pady=(3, 0))
        self.inputs.append(selection)
        ttk.Label(outer, text="Jukebox 3.8.0 is required. Install it through Geode in GD.\nGeode >=5.10.1 and <6.0.0 must already be installed for Proton.", wraplength=680).grid(row=4, column=0, sticky="w", pady=(12, 8))
        choices = ttk.Frame(outer)
        choices.grid(row=5, column=0, sticky="ew")
        reopen = ttk.Checkbutton(choices, text="Reopen GD and OBS afterward", variable=self.reopen)
        reopen.pack(anchor="w")
        self.inputs.append(reopen)
        ttk.Label(choices, text="After installation, add GD Sounds to your OBS scenes.").pack(anchor="w", pady=(6, 0))
        advanced = ttk.Frame(outer)
        advanced.grid(row=6, column=0, sticky="ew", pady=(8, 0))
        advanced.columnconfigure(0, weight=1)
        self.path_row(advanced, 0, "Proton prefix (optional; detected automatically)", self.prefix)
        outer.rowconfigure(7, weight=1)
        self.status_label = ttk.Label(outer, textvariable=self.status, style="Status.TLabel", wraplength=678, justify="left")
        self.status_label.grid(row=7, column=0, sticky="sw", pady=(20, 18))
        buttons = ttk.Frame(outer)
        buttons.grid(row=8, column=0, sticky="w")
        self.install_button = ttk.Button(buttons, text="Install", command=self.install)
        self.install_button.pack(side="left")
        self.inputs.append(self.install_button)
        undo = ttk.Button(buttons, text="Undo last install", command=lambda: self.install(undo=True))
        undo.pack(side="left", padx=(10, 0))
        self.inputs.append(undo)
        logs = tk.Label(outer, text="Open backups and setup logs", font=("DejaVu Sans", 10), fg="#0066cc", bg="#f0f0f0", cursor="hand2", underline=0)
        logs.grid(row=9, column=0, sticky="w", pady=(12, 0))
        logs.bind("<Button-1>", lambda _: self.open_logs())
        logs.bind("<Return>", lambda _: self.open_logs())
        logs.configure(takefocus=True)
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(75, self.poll)

    def path_row(self, parent, row, label, variable):
        frame = ttk.Frame(parent)
        frame.grid(row=row, column=0, sticky="ew", pady=6)
        frame.columnconfigure(0, weight=1)
        ttk.Label(frame, text=label).grid(row=0, column=0, sticky="w")
        entry = ttk.Entry(frame, textvariable=variable)
        entry.grid(row=1, column=0, sticky="ew", pady=(3, 0))
        def browse():
            selected = filedialog.askdirectory(parent=self.root, title="Choose " + label, initialdir=variable.get() or str(Path.home()))
            if selected:
                variable.set(selected)
        button = ttk.Button(frame, text="Browse...", command=browse)
        button.grid(row=1, column=1, padx=(8, 0), pady=(3, 0))
        self.inputs.extend((entry, button))

    def set_busy(self, value):
        self.busy = value
        for control in self.inputs:
            control.configure(state="disabled" if value else "readonly" if isinstance(control, ttk.Combobox) else "normal")

    def install(self, undo=False):
        if self.busy:
            return
        try:
            command = ["bash", str(self.package / "Install.sh"), "--uninstall"] if undo else install_command(self.package, self.game.get(), self.prefix.get(), self.obs.get() == "Flatpak OBS Studio")
            logs = data_root() / "SetupLogs"
            logs.mkdir(parents=True, exist_ok=True)
            log = logs / (time.strftime("%Y%m%d-%H%M%S") + "-" + str(time.time_ns()) + ".log")
        except (ValueError, OSError) as error:
            self.status.set(str(error))
            return
        self.undo_after = undo
        self.reopen_after = self.reopen.get() and not undo
        self.flatpak_after = self.obs.get() == "Flatpak OBS Studio"
        self.set_busy(True)
        self.status.set("Undoing last install..." if undo else "Installing OBS Jukebox...")
        threading.Thread(target=self.run_backend, args=(command, log), daemon=True).start()

    def run_backend(self, command, log):
        last_line = "Installation did not finish. Open setup logs for details."
        try:
            with log.open("w", encoding="utf-8") as output:
                with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace") as process:
                    for line in process.stdout:
                        output.write(line)
                        output.flush()
                        if line.strip():
                            last_line = line.strip()
                    code = process.wait()
            self.events.put((code == 0, last_line))
        except OSError as error:
            self.events.put((False, str(error)))

    def poll(self):
        try:
            success, detail = self.events.get_nowait()
        except queue.Empty:
            pass
        else:
            self.set_busy(False)
            if success:
                self.status.set(detail if self.undo_after else "Installed. In OBS, add GD Sounds once. Keep monitoring off and exclude GD audio from other recording sources.")
                if self.reopen_after:
                    try:
                        obs_command = ["flatpak", "run", "com.obsproject.Studio"] if self.flatpak_after else ["obs"]
                        subprocess.Popen(obs_command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                        subprocess.Popen(["xdg-open", "steam://rungameid/322170"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                    except OSError as error:
                        self.status.set("Installed. Reopen GD and OBS manually: " + str(error))
            else:
                self.status.set(detail + " Open setup logs for details.")
        self.root.after(75, self.poll)

    def open_logs(self):
        try:
            data_root().mkdir(parents=True, exist_ok=True)
            subprocess.Popen(["xdg-open", str(data_root())], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as error:
            self.status.set("Could not open setup logs: " + str(error))

    def close(self):
        if self.busy:
            messagebox.showinfo("Installation in progress", "Wait for installation to finish before closing setup.", parent=self.root)
        else:
            self.root.destroy()


if __name__ == "__main__":
    window = tk.Tk()
    SetupWindow(window, Path(__file__).resolve().parent)
    window.mainloop()
