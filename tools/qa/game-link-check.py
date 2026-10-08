import argparse
import json
from pathlib import Path
import socket
import time

from link_protocol import AGE_SCOPE, now_ns, parse_packet

parser = argparse.ArgumentParser(description="Observe real game packets without starting the game or OBS")
parser.add_argument("--seconds", type=float, default=5)
parser.add_argument("--port", type=int, default=39022)
parser.add_argument("--output", type=Path, default=Path("artifacts/windows-validation/live-game-link.json"))
args = parser.parse_args()
if not 0 < args.seconds <= 30 or not 1024 <= args.port <= 65535:
    parser.error("duration must be (0,30] seconds and port must be 1024..65535")
states = []
previous = {}
song_count = effects_count = clock_count = invalid_count = 0
effects_frames = 0
effects_peak = 0.0
effects_nonzero_packets = 0
audible_ages, ages_ms = [], []
rates, statuses, versions, channels, sessions = set(), set(), set(), set(), set()
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
    receiver.bind(("127.0.0.1", args.port))
    receiver.settimeout(.2)
    started = time.monotonic()
    while time.monotonic() - started < args.seconds:
        try:
            packet, peer = receiver.recvfrom(8192)
        except socket.timeout:
            continue
        try:
            kind, fields = parse_packet(packet)
        except ValueError:
            invalid_count += 1
            continue
        if kind == "song":
            song_count += 1
            versions.add(fields["version"])
            channels.add(fields["channel_id"])
            sessions.add(fields["session_id"])
            statuses.add(fields["status"])
            key = fields["session_id"], fields["channel_id"]
            tracked = tuple(fields[k] for k in ("flags", "epoch", "status", "song", "obs_music_volume", "obs_effects_volume", "trigger_gain", "fades", "loop_start", "loop_end"))
            if previous.get(key) != tracked:
                states.append(fields)
                previous[key] = tracked
        elif kind == "effects":
            age = (now_ns() - fields["timestamp_ns"]) / 1_000_000
            effects_count += 1
            peak = max(map(abs, fields["samples"]))
            effects_peak = max(effects_peak, peak)
            effects_nonzero_packets += peak > 0.00001
            if peak > 0.00001:
                audible_ages.append(age)
            effects_frames += fields["frames"]
            rates.add(fields["rate"])
            ages_ms.append(age)
        else:
            clock_count += 1
    duration = time.monotonic() - started
result = dict(scope="Observed UDP traffic; no synthetic sender created; does not prove OBS playback or real gameplay transitions",
              port=args.port, observed_seconds=duration, song_packets=song_count, song_packets_per_second=song_count / duration,
              song_versions=sorted(versions), channel_ids=sorted(channels), session_ids=sorted(sessions), clock_packets=clock_count,
              effects_packets=effects_count, effects_frames=effects_frames, effects_sample_rates=sorted(rates),
              effects_peak=effects_peak, effects_nonzero_packets=effects_nonzero_packets,
              timestamp_age_scope=AGE_SCOPE,
              audible_age_ms_min=min(audible_ages, default=None), audible_age_ms_max=max(audible_ages, default=None),
              raw_audible_clock_differences_above_40ms=sum(age > 40 for age in audible_ages),
              effects_timestamp_age_ms_min=min(ages_ms, default=None), effects_timestamp_age_ms_max=max(ages_ms, default=None),
              invalid_packets=invalid_count, statuses=sorted(statuses), state_changes=states,
              pass_valid_song_heartbeat=song_count > 0 and invalid_count == 0)
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["pass_valid_song_heartbeat"] else 1)
