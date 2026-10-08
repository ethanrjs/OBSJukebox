import argparse
import json
from pathlib import Path
import socket
import struct
import time

p = argparse.ArgumentParser()
p.add_argument("--output", type=Path, required=True)
p.add_argument("--seconds", type=float, default=45)
args = p.parse_args()
if not 1 <= args.seconds <= 60:
    p.error("bounded to 1..60 seconds")
args.output.mkdir(parents=True, exist_ok=True)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s, (args.output / "packets.jsonl").open("w") as out:
    s.bind(("127.0.0.1", 39032))
    s.settimeout(.1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2**20)
    start = time.monotonic()
    sender = None
    songs = effects = nonzero = clock = 0
    while time.monotonic() - start < args.seconds:
        try:
            data, peer = s.recvfrom(8192)
        except socket.timeout:
            continue
        row = dict(at=time.monotonic() - start, bytes=len(data))
        if data[:8] == b"GDCLK1\0\0":
            kind = struct.unpack_from("<I", data, 12)[0]
            if kind == 1 and sender:
                s.sendto(data, sender)
            elif kind == 2:
                s.sendto(data, ("127.0.0.1", 39033))
            clock += 1
            continue
        if data[:8] == b"GDSONG1\0":
            sender = peer
            version, flags, epoch, attempt = struct.unpack_from("<IIIi", data, 8)
            position, rate, offset = struct.unpack_from("<ddd", data, 24)
            row.update(kind="song", version=version, flags=flags, epoch=epoch, attempt=attempt,
                       position=position, rate=rate, offset=offset,
                       status=data[48:72].split(b"\0")[0].decode(),
                       path=data[264:1288].split(b"\0")[0].decode())
            if version == 5 and len(data) >= 1420:
                timestamp, session, channel, gain, fades = struct.unpack_from("<QQifI", data, 1296)
                row.update(timestamp=timestamp, session=session, channel=channel, gain=gain, fades=fades)
            songs += 1
        elif data[:6] == b"GDSFX2":
            frames = struct.unpack_from("<I", data, 20)[0]
            values = struct.unpack_from(f"<{frames * 2}f", data, 44)
            peak = max(map(abs, values), default=0)
            row.update(kind="sfx", frames=frames, peak=peak)
            effects += 1
            nonzero += peak > 1e-5
        else:
            row.update(kind="other")
        out.write(json.dumps(row) + "\n")
        s.sendto(data, ("127.0.0.1", 39033))
    summary = dict(song_packets=songs, effects_packets=effects, nonzero_effect_packets=nonzero, clock_packets=clock)
    (args.output / "relay-results.json").write_text(json.dumps(summary, indent=2))
    print(summary)
