import argparse
import ctypes
import json
import math
from pathlib import Path
import socket
import struct
import time

parser = argparse.ArgumentParser()
parser.add_argument("--seconds", type=float, default=15)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if not 1 <= args.seconds <= 60:
    parser.error("seconds must be between 1 and 60")

frequency = ctypes.c_longlong()
ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(frequency))

def now_ns():
    value = ctypes.c_longlong()
    ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value))
    return value.value * 1_000_000_000 // frequency.value

header = struct.Struct("<8sIIIIQI")
songs = effects = nonzero_packets = nonzero_samples = invalid = 0
ages = []
nonzero_ages = []
rates = set()
peak = 0.0
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.bind(("127.0.0.1", 39022))
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    sock.settimeout(.1)
    print("Relaying real GD effects 39022 -> 39023; stripping song paths only", flush=True)
    start = time.monotonic()
    while time.monotonic() - start < args.seconds:
        try:
            packet, _ = sock.recvfrom(8192)
        except socket.timeout:
            continue
        if len(packet) in (1288, 1296) and packet[:8] == b"GDSONG1\0":

            packet = packet[:264] + bytes(1024) + packet[1288:]
            songs += 1
        elif len(packet) >= header.size and packet[:8] == b"GDSFX1\0\0":
            _, version, sequence, rate, frames, timestamp, stream = header.unpack_from(packet)
            if version != 1 or not 1 <= frames <= 512 or len(packet) != 36 + frames * 8 or not 8000 <= rate <= 192000:
                invalid += 1
                continue
            values = struct.unpack_from(f"<{frames * 2}f", packet, 36)
            if not all(map(math.isfinite, values)):
                invalid += 1
                continue
            age = (now_ns() - timestamp) / 1e6
            effects += 1
            ages.append(age)
            rates.add(rate)
            count = sum(abs(v) > 1e-5 for v in values)
            peak = max(peak, max(map(abs, values)))
            if count:
                nonzero_packets += 1
                nonzero_samples += count
                nonzero_ages.append(age)
            
        else:
            invalid += 1
            continue
        sock.sendto(packet, ("127.0.0.1", 39023))

result = dict(scope="Actual GD packets relayed; song path cleared only; effects bytes unchanged; not physical listening",
              seconds=time.monotonic() - start, song_packets=songs, effects_packets=effects,
              nonzero_effect_packets=nonzero_packets, nonzero_samples=nonzero_samples,
              peak=peak, rates=sorted(rates), invalid_packets=invalid,
              age_ms_min=min(ages, default=None), age_ms_max=max(ages, default=None),
              nonzero_age_ms_min=min(nonzero_ages, default=None), nonzero_age_ms_max=max(nonzero_ages, default=None),
              nonzero_packets_older_than_40ms=sum(age > 40 for age in nonzero_ages))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, indent=2))
