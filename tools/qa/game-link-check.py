import argparse
import ctypes
import json
import math
from pathlib import Path
import socket
import struct
import time

parser = argparse.ArgumentParser()
parser.add_argument("--seconds", type=float, default=5)
parser.add_argument("--output", type=Path, default=Path("artifacts/windows-validation/live-game-link.json"))
args = parser.parse_args()
if not 0 < args.seconds <= 30:
    parser.error("duration must be between 0 and 30 seconds")
frequency = ctypes.c_longlong()
ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))
def qpc_ns():
    ticks = ctypes.c_longlong()
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(ticks))
    return ticks.value * 1_000_000_000 // frequency.value

song_layout = struct.Struct("<8sIIIiddd24s96s96s1024s")
effects_header = struct.Struct("<8sIIIIQI")
states = []
song_count = effects_count = invalid_count = 0
effects_frames = 0
effects_peak = 0.0
effects_nonzero_packets = 0
audible_ages = []
ages_ms = []
rates = set()
statuses = set()
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
    
    receiver.bind(("127.0.0.1", 39022))
    receiver.settimeout(.2)
    started = time.monotonic()
    while time.monotonic() - started < args.seconds:
        try:
            packet, peer = receiver.recvfrom(8192)
        except socket.timeout:
            continue
        if len(packet) in (song_layout.size, song_layout.size + 8) and packet[:8] == b"GDSONG1\0":
            _, version, flags, epoch, attempt, position, rate, offset, status, level, song, path = song_layout.unpack_from(packet)
            volumes = struct.unpack_from('<ff', packet, song_layout.size) if version == 3 and len(packet) == song_layout.size + 8 else (1., 1.)
            if (version, len(packet)) not in ((2, 1288), (3, 1296)) or not all(map(math.isfinite, (position, rate, offset, *volumes))) or not .25 <= rate <= 4 or not all(0 <= v <= 1 for v in volumes):
                invalid_count += 1
                continue
            song_count += 1
            text = lambda value: value.split(b"\0", 1)[0].decode("utf-8", errors="replace")
            state = dict(flags=flags, epoch=epoch, attempt=attempt, position=position, rate=rate, offset=offset,
                         status=text(status), level=text(level), song=text(song), song_path_present=bool(text(path)), obs_music_volume=volumes[0], obs_effects_volume=volumes[1])
            statuses.add(state["status"])
            if not states or any(state[k] != states[-1][k] for k in ("flags", "epoch", "status", "song", "obs_music_volume", "obs_effects_volume")):
                states.append(state)
        elif len(packet) >= effects_header.size and packet[:8] == b"GDSFX1\0\0":
            _, version, sequence, rate, frames, timestamp, stream = effects_header.unpack_from(packet)
            age = (qpc_ns() - timestamp) / 1_000_000
            if version != 1 or not 1 <= frames <= 512 or len(packet) != 36 + frames * 8 or not 8000 <= rate <= 192000 or abs(age) > 1000 or stream > 1:
                invalid_count += 1
                continue
            values = struct.unpack_from(f"<{frames * 2}f", packet, 36)
            if not all(map(math.isfinite, values)):
                invalid_count += 1
                continue
            effects_count += 1
            peak = max(map(abs, values), default=0.0)
            effects_peak = max(effects_peak, peak)
            effects_nonzero_packets += peak > 0.00001
            if peak > 0.00001: audible_ages.append(age)
            effects_frames += frames
            rates.add(rate)
            ages_ms.append(age)
        else:
            invalid_count += 1
    duration = time.monotonic() - started
result = dict(scope="Actual running Geometry Dash UDP packets; no synthetic sender; does not prove OBS playback or real gameplay transitions",
              observed_seconds=duration, song_packets=song_count, song_packets_per_second=song_count / duration,
              effects_packets=effects_count, effects_frames=effects_frames, effects_sample_rates=sorted(rates),
              effects_peak=effects_peak, effects_nonzero_packets=effects_nonzero_packets,
              audible_age_ms_min=min(audible_ages) if audible_ages else None,
              audible_age_ms_max=max(audible_ages) if audible_ages else None,
              audible_packets_older_than_40ms=sum(age > 40 for age in audible_ages),
              effects_timestamp_age_ms_min=min(ages_ms) if ages_ms else None,
              effects_timestamp_age_ms_max=max(ages_ms) if ages_ms else None,
              invalid_packets=invalid_count, statuses=sorted(statuses), state_changes=states,
              pass_valid_song_heartbeat=song_count > 0 and invalid_count == 0)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["pass_valid_song_heartbeat"] else 1)
