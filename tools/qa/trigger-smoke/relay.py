import argparse
import json
from pathlib import Path
import socket
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from link_protocol import parse_packet, RelayRoutes, SONG

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
p.add_argument('--seconds', type=float, default=45)
args = p.parse_args()
if not 1 <= args.seconds <= 60:
    p.error('bounded to 1..60 seconds')
args.output.mkdir(parents=True, exist_ok=True)
routes = RelayRoutes(('127.0.0.1', 39033))
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s, (args.output / 'packets.jsonl').open('w') as out:
    s.bind(('127.0.0.1', 39032))
    s.settimeout(.1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2**20)
    start = time.monotonic()
    songs = effects = nonzero = clock = invalid = 0
    while time.monotonic() - start < args.seconds:
        try:
            data, peer = s.recvfrom(8192)
        except socket.timeout:
            continue
        try:
            kind, fields = parse_packet(data)
        except ValueError:
            invalid += 1
            continue
        destination = routes.route(kind, fields, peer)
        if destination is None:
            continue
        if kind == 'clock':
            clock += 1
        else:
            row = dict(at=time.monotonic() - start, bytes=len(data), **fields)
            if kind == 'song':
                songs += 1
                row.update(kind='song', path=SONG.unpack_from(data)[-1].split(b'\0', 1)[0].decode('utf-8', errors='replace'), timestamp=fields['timestamp_ns'], session=fields['session_id'],
                           channel=fields['channel_id'], gain=fields['trigger_gain'], fades=len(fields['fades']))
            else:
                samples = row.pop('samples')
                peak = max(map(abs, samples), default=0)
                row.update(kind='sfx', peak=peak)
                effects += 1
                nonzero += peak > 1e-5
            out.write(json.dumps(row) + '\n')
        s.sendto(data, destination)
    summary = dict(song_packets=songs, effects_packets=effects, nonzero_effect_packets=nonzero,
                   clock_packets=clock, invalid_packets=invalid)
    (args.output / 'relay-results.json').write_text(json.dumps(summary, indent=2))
    print(summary)
