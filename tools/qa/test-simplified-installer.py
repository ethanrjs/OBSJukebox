import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "artifacts/windows/installer-simplified-test"
JUKebox_SHA = "A0ECA6C82C7FA6149B956A809A9058959957D7C4672368B6933EA030E641B621"
parser = argparse.ArgumentParser()
parser.add_argument("--exe", type=Path, default=ROOT / "artifacts/windows/release/OBS-Jukebox-1.0.0-Windows-Setup.exe")
args = parser.parse_args()
exe = args.exe.resolve()
run = OUTPUT / (datetime.now().strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:6])
run.mkdir(parents=True)
records = []
checks = []
expected_mod = None
expected_plugin = None

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()

def snapshot(root):
    return {"files": {str(p.relative_to(root)): digest(p) for p in sorted(root.rglob("*")) if p.is_file()},
            "directories": [str(p.relative_to(root)) for p in sorted(root.rglob("*")) if p.is_dir()]}

def check(condition, description):
    checks.append({"check": description, "pass": bool(condition)})
    if not condition:
        raise AssertionError(description)

def seed(name, jukebox=False, renamed=False, compatible=True):
    base = run / name
    (base / "gd").mkdir(parents=True)
    (base / "obs/bin/64bit").mkdir(parents=True)
    (base / "scenes").mkdir()
    (base / "gd/GeometryDash.exe").write_bytes(b"QA placeholder; never executed")
    (base / "obs/bin/64bit/obs64.exe").write_bytes(b"QA placeholder; never executed")
    shutil.copyfile(ROOT / "artifacts/windows/installer-final-test/scenes/Original.json", base / "scenes/Original.json")
    if compatible:
        shutil.copyfile(ROOT / "tools/geode-windows-5.10.1/Geode.dll", base / "gd/Geode.dll")
    (base / "gd/geode/mods").mkdir(parents=True)
    with zipfile.ZipFile(base / "gd/geode/mods/unrelated.geode", "w") as unrelated:
        unrelated.writestr("mod.json", json.dumps({"id": "qa.unrelated", "version": "1.0.0", "name": "Unrelated QA fixture"}))
        unrelated.writestr("qa.unrelated.dll", b"placeholder; never loaded")
    if jukebox:
        name = "renamed-jukebox-package.geode" if renamed else "fleym.nongd.geode"
        shutil.copyfile(ROOT / "artifacts/windows/fleym.nongd.geode", base / "gd/geode/mods" / name)
    return base

def invoke(base, label, extra=(), success=True):
    arguments = [str(exe), "--silent", "--gd", str(base / "gd"), "--obs", str(base / "obs"),
                 "--plugin-root", str(base / "plugins"), "--state-root", str(base / "state"),
                 "--scene-root", str(base / "scenes"), *extra]
    check("--close-apps" not in arguments and "--restart" not in arguments, label + ": no live-process action flags")
    result = subprocess.run(arguments, capture_output=True, text=True, errors="replace", timeout=120,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    (run / (label + ".stdout.txt")).write_text(result.stdout, encoding="utf-8")
    (run / (label + ".stderr.txt")).write_text(result.stderr, encoding="utf-8")
    records.append({"label": label, "fixture": str(base), "arguments": arguments[1:], "exitCode": result.returncode,
                    "stdout": str(run / (label + ".stdout.txt")), "stderr": str(run / (label + ".stderr.txt"))})
    check((result.returncode == 0) if success else (result.returncode != 0), label + ": expected exit status")
    print(label, "exit", result.returncode, flush=True)
    return result.stdout + result.stderr

def installed(base):
    check(digest(base / "gd/geode/mods/local.separate_song.geode") == expected_mod, "exact final mod payload")
    check(digest(base / "plugins/separate-song/bin/64bit/separate-song.dll") == expected_plugin, "exact final OBS DLL payload")
    old = json.loads((ROOT / "artifacts/windows/installer-final-test/scenes/Original.json").read_text())
    current = json.loads((base / "scenes/Original.json").read_text())
    for key in ("DesktopAudioDevice1", "AuxAudioDevice1", "modules", "current_scene"):
        check(current[key] == old[key], "preserve collection field " + key)
    songs = [s for s in current["sources"] if s.get("id") == "gd_alternate_song"]
    check(len(songs) == 1 and songs[0]["name"] == "GD Sounds", "one combined GD Sounds source")
    song = songs[0]
    check(song["uuid"] == "old-song-id" and song["volume"] == .67 and song["mixers"] == 2 and song["filters"] == [{"name": "My filter"}], "preserve existing song UUID volume tracks filters")
    for scene in (s for s in current["sources"] if s.get("id") == "scene"):
        check(sum(i.get("source_uuid") == song["uuid"] for i in scene["settings"]["items"]) == 1, "shared source once in " + scene["name"])

def manifest(base):
    return Path((base / "state/latest-manifest.txt").read_text().strip())

report = {"scope": "Packaged installer staged CLI tests; fake GD/OBS roots only; no live apps, UI acceptance, or UDP",
          "generatedAt": datetime.now(timezone.utc).isoformat(), "exe": str(exe), "exeSha256": digest(exe),
          "exeBytes": exe.stat().st_size, "runRoot": str(run)}
expected_mod = digest(ROOT / "artifacts/windows/local.separate_song.geode")
expected_plugin = digest(ROOT / "artifacts/windows/separate-song.dll")
report["expectedModSha256"] = expected_mod
report["expectedPluginSha256"] = expected_plugin
try:
    with zipfile.ZipFile(ROOT / "installer/windows/payload.zip") as payload:
        entries = payload.namelist()
        check("mods/fleym.nongd.geode" not in entries, "packaging input payload does not bundle Jukebox")
        report["payloadZipSha256"] = digest(ROOT / "installer/windows/payload.zip")
        report["payloadZipEntries"] = entries

    base = seed("preinstalled", jukebox=True, renamed=True)
    before = snapshot(base)
    original_scene = (base / "scenes/Original.json").read_bytes()
    jukebox_file = base / "gd/geode/mods/renamed-jukebox-package.geode"
    kept_jukebox = digest(jukebox_file)
    loader = digest(base / "gd/Geode.dll")
    invoke(base, "preinstalled-install")
    installed(base)
    check(digest(jukebox_file) == kept_jukebox and not (base / "gd/geode/mods/fleym.nongd.geode").exists(), "renamed installed Jukebox preserved with no duplicate")
    check(digest(base / "gd/Geode.dll") == loader, "compatible Geode preserved")
    after = snapshot(base)
    saved_manifest = manifest(base)
    invoke(base, "preinstalled-idempotent")
    check(snapshot(base) == after and manifest(base) == saved_manifest, "idempotent reinstall does not change files or restoration manifest")
    invoke(base, "preinstalled-undo", ("--uninstall",))
    check((base / "scenes/Original.json").read_bytes() == original_scene, "undo restores original scene bytes")
    check(all((base / p).exists() and digest(base / p) == h for p, h in before["files"].items()), "undo restores/preserves all original fixture file hashes")
    check(not (base / "gd/geode/mods/local.separate_song.geode").exists() and not (base / "plugins/separate-song/bin/64bit/separate-song.dll").exists(), "undo removes unchanged files created by installer")

    base = seed("missing", compatible=False)
    before = snapshot(base)
    declined = invoke(base, "missing-without-optin", success=False)
    check("Jukebox" in declined and snapshot(base) == before, "missing dependency without opt-in fails before any fixture writes")
    invoke(base, "missing-optin-dry-run", ("--dry-run", "--install-jukebox"))
    check(snapshot(base) == before, "opt-in dry run makes no fixture writes")
    downloaded = invoke(base, "missing-optin-install", ("--install-jukebox",))
    installed(base)
    check(digest(base / "gd/geode/mods/unrelated.geode") == before["files"][str(Path("gd/geode/mods/unrelated.geode"))], "fresh Geode and dependency install preserve unrelated mod")
    check(digest(base / "gd/geode/mods/fleym.nongd.geode") == JUKebox_SHA, "opt-in installed official Jukebox matches pinned SHA256")
    check((base / "gd/Geode.dll").exists() and (base / "gd/geode/resources/geode.loader/version").exists(), "fresh missing Geode receives complete loader resources")
    report["officialJukeboxUrl"] = "https://github.com/Fleeym/jukebox/releases/download/v3.8.0/fleym.nongd.geode"
    report["officialJukeboxSha256"] = JUKebox_SHA
    after = snapshot(base)
    saved_manifest = manifest(base)
    invoke(base, "downloaded-idempotent")
    check(snapshot(base) == after and manifest(base) == saved_manifest, "downloaded dependency reinstall is idempotent")

    base = seed("hash-safe-undo", jukebox=True)
    invoke(base, "hash-safe-install")
    plugin = base / "plugins/separate-song/bin/64bit/separate-song.dll"
    plugin.write_bytes(b"later user plugin edit")
    scene = base / "scenes/Original.json"
    edited = json.loads(scene.read_text()); edited["modules"]["userLaterEdit"] = {"value": 9001}
    scene.write_text(json.dumps(edited), encoding="utf-8")
    plugin_hash, scene_hash = digest(plugin), digest(scene)
    invoke(base, "hash-safe-undo", ("--uninstall",))
    check(digest(plugin) == plugin_hash and digest(scene) == scene_hash, "undo preserves later plugin and scene edits by installed-hash checks")
    check(not (base / "gd/geode/mods/local.separate_song.geode").exists(), "undo still removes unchanged owned mod")

    base = seed("empty-undo", jukebox=True)
    before = snapshot(base)
    empty = invoke(base, "empty-undo", ("--uninstall",))
    check(snapshot(base) == before, "Undo with no manifest does not write fixture files")
    check("--manifest" not in empty and "no" in empty.lower(), "Undo with no manifest gives friendly explanation")
    check(digest(exe) == report["exeSha256"], "tested packaged EXE unchanged throughout staged verification")
    report["result"] = "PASS"
except Exception as error:
    report["result"] = "FAIL"
    report["error"] = str(error)
finally:
    report["cases"] = records
    report["checks"] = checks
    (run / "RESULT.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (OUTPUT / "RESULT.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"result": report["result"], "error": report.get("error"), "report": str(run / "RESULT.json")}, indent=2), flush=True)
raise SystemExit(0 if report["result"] == "PASS" else 1)
