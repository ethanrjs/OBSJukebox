import argparse
import collections
import json
import math
from pathlib import Path
import struct

p = argparse.ArgumentParser()
p.add_argument("directory", type=Path)
args = p.parse_args()
rows = [json.loads(line) for line in (args.directory / "packets.jsonl").read_text().splitlines()]
songs = [r for r in rows if r["kind"] == "song"]
channels = collections.defaultdict(list)
for row in songs:
    channels[row.get("channel", 0)].append(row)
checks = {}
driver = (args.directory / "trigger-smoke.log").read_text()
checks["isolated_driver_completed"] = "ISOLATION_OK" in driver and "FINISHED" in driver
checks["actual_pause_resume_control"] = "actual_pause_layer=1" in driver
checks["v5_native_sender"] = bool(songs) and all(r["version"] == 5 for r in songs)
checks["three_music_channels"] = {0, 1, 2}.issubset(channels)
checks["channel_1_source_switch"] = len({r["path"] for r in channels[1] if r["path"]}) >= 2
checks["song_trigger_volume_0_7"] = any(abs(r.get("gain", 1) - .7) < .001 for r in channels[1])
checks["edit_song_volume_0_2"] = any(abs(r.get("gain", 1) - .2) < .005 for r in channels[1])
checks["native_fade_points"] = any(r.get("fades", 0) for r in songs)
checks["pause_event"] = any(r["status"] == "Paused" and not r["flags"] & 2 for r in songs)
deaths = [r for r in songs if r["status"] == "Death"]
checks["death_pauses_channel_0"] = bool(deaths) and all(
    any(abs(r["at"] - d["at"]) < .04 and not r["flags"] & 2 for r in channels[0]) for d in deaths)
checks["restart_attempts"] = len({r["attempt"] for r in songs if r["attempt"]}) >= 2
paused = [r for r in channels[0] if not r["flags"] & 2 and r["position"] > 1.5]
checks["long_replacement_after_original_eof"] = any(r["flags"] & 2 and r["position"] > 2 and r["path"].endswith("obs-long.wav") for r in channels[0])
checks["long_replacement_resumes_after_eof_pause"] = any(
    r["at"] > q["at"] + .5 and r["attempt"] == q["attempt"] and r["flags"] & 2 and
    r["position"] >= q["position"] and r["path"].endswith("obs-long.wav") for q in paused for r in channels[0])
checks["sfx_while_game_sfx_zero"] = any(r["kind"] == "sfx" and r["peak"] > 1e-5 for r in rows)
wav = (args.directory / "live-source-left.wav").read_bytes()
data_at = wav.index(b"data") + 8
samples = struct.unpack_from(f"<{(len(wav)-data_at)//4}f", wav, data_at)
checks["finite_pcm"] = bool(samples) and all(map(math.isfinite, samples))
blocks = []
size = 9600
for start in range(0, len(samples) - size, size):
    block = samples[start:start + size]
    amplitudes = {}
    for hz in (330, 550, 770):
        step = 2 * math.pi * hz / 48000
        re = sum(v * math.cos(i * step) for i, v in enumerate(block))
        im = sum(v * math.sin(i * step) for i, v in enumerate(block))
        amplitudes[hz] = math.hypot(re, im) * 2 / size
    blocks.append(dict(seconds=start / 48000, amplitudes=amplitudes,
                       rms=math.sqrt(sum(v*v for v in block)/size)))
for hz in (330, 550, 770):
    checks[f"pcm_{hz}_hz_song"] = any(b["amplitudes"][hz] > .015 for b in blocks)
checks["three_simultaneous_pcm_songs"] = any(all(b["amplitudes"][hz] > .015 for hz in (330, 550, 770)) for b in blocks)
checks["edit_song_volume_in_native_pcm"] = any(
    .032 < b["amplitudes"][550] < .042 and b["amplitudes"][330] > .16 and b["amplitudes"][770] > .15
    for b in blocks)
summary = dict(scope="Actual native GD triggers, original packets and native OBS PCM; no physical listening or UI capture",
               checks=checks, passed=sum(checks.values()), total=len(checks),
               channels={c: dict(packets=len(rs), paths=sorted({r["path"] for r in rs if r["path"]}),
                                 gains=sorted({round(r.get("gain", 1), 3) for r in rs})) for c, rs in channels.items()})
(args.directory / "analysis.json").write_text(json.dumps(summary, indent=2))
(args.directory / "spectral-blocks.json").write_text(json.dumps(blocks, indent=2))
print(json.dumps(summary, indent=2))
raise SystemExit(0 if all(checks.values()) else 1)
