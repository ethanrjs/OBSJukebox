import argparse
import json
from pathlib import Path
import socket
import time

from link_protocol import AGE_SCOPE, RelayRoutes, now_ns, parse_packet

parser = argparse.ArgumentParser(description="Relay game effects to a test OBS receiver; clears song paths")
parser.add_argument("--seconds", type=float, default=15)
parser.add_argument("--listen-port", type=int, default=39022)
parser.add_argument("--destination-port", type=int, default=39023)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
if not 1 <= args.seconds <= 60:
    parser.error("seconds must be between 1 and 60")
if not all(1024 <= p <= 65535 for p in (args.listen_port, args.destination_port)) or args.listen_port == args.destination_port:
    parser.error("ports must be distinct and in 1024..65535")

routes = RelayRoutes(("127.0.0.1", args.destination_port))
songs = effects = nonzero_packets = nonzero_samples = invalid = unroutable = probes = replies = modern_songs = 0
ages, nonzero_ages = [], []
rates = set()
peak = 0.0
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.bind(("127.0.0.1", args.listen_port))
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    sock.settimeout(.1)
    print(f"Relaying GD effects {args.listen_port} -> {args.destination_port}; stripping song paths only", flush=True)
    start = time.monotonic()
    while time.monotonic() - start < args.seconds:
        try:
            packet, peer = sock.recvfrom(8192)
        except socket.timeout:
            continue
        try:
            kind, fields = parse_packet(packet)
        except ValueError:
            invalid += 1
            continue
        destination = routes.route(kind, fields, peer)
        if destination is None:
            unroutable += 1
            continue
        if kind == "song":
            packet = packet[:264] + bytes(1024) + packet[1288:]
            songs += 1
            modern_songs += fields["version"] == 5
        elif kind == "effects":
            age = (now_ns() - fields["timestamp_ns"]) / 1e6
            effects += 1
            ages.append(age)
            rates.add(fields["rate"])
            count = sum(abs(v) > 1e-5 for v in fields["samples"])
            peak = max(peak, max(map(abs, fields["samples"])))
            if count:
                nonzero_packets += 1
                nonzero_samples += count
                nonzero_ages.append(age)
        else:
            probes += fields["kind"] == 1
            replies += fields["kind"] == 2
        sock.sendto(packet, destination)

result = dict(scope="Observed UDP packets relayed; song paths cleared; effects/clock bytes unchanged; not physical listening",
              seconds=time.monotonic() - start, listen_port=args.listen_port, destination_port=args.destination_port,
              song_packets=songs, effects_packets=effects, clock_probes_forwarded=probes, clock_replies_forwarded=replies,
              nonzero_effect_packets=nonzero_packets, nonzero_samples=nonzero_samples,
              peak=peak, rates=sorted(rates), invalid_packets=invalid, unroutable_packets=unroutable,
              timestamp_age_scope=AGE_SCOPE, age_ms_min=min(ages, default=None), age_ms_max=max(ages, default=None),
              nonzero_age_ms_min=min(nonzero_ages, default=None), nonzero_age_ms_max=max(nonzero_ages, default=None),
              raw_nonzero_clock_differences_above_40ms=sum(age > 40 for age in nonzero_ages),
              pass_relay=songs > 0 and effects > 0 and nonzero_packets > 0 and invalid == 0 and (not modern_songs or replies > 0))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["pass_relay"] else 1)
