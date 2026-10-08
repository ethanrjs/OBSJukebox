import ctypes
import math
import struct
import sys
import time

SONG = struct.Struct("<8sIIIiddd24s96s96s1024s")
CLOCK = struct.Struct("<8sIIQQQQ")
SONG_SIZES = {2: 1288, 3: 1296, 4: 1304, 5: 1436}


if sys.platform == "win32":
    _frequency = ctypes.c_longlong()
    if not ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(_frequency)):
        raise OSError("QueryPerformanceFrequency failed")

    def now_ns():
        value = ctypes.c_longlong()
        if not ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(value)):
            raise OSError("QueryPerformanceCounter failed")
        return value.value * 1_000_000_000 // _frequency.value
else:
    now_ns = time.monotonic_ns


AGE_SCOPE = ("raw foreign-clock difference; not delivery latency (Wine clock may differ)"
             if sys.platform.startswith("linux") else
             "raw native-clock difference; assumes sender uses this host's native clock")


def parse_packet(packet):
    magic = packet[:8]
    if magic == b"GDSONG1\0":
        if len(packet) < SONG.size:
            raise ValueError("truncated song packet")
        _, version, flags, epoch, attempt, position, rate, offset, status, level, song, path = SONG.unpack_from(packet)
        if SONG_SIZES.get(version) != len(packet):
            raise ValueError("song version/size mismatch")
        volumes = struct.unpack_from("<ff", packet, 1288) if version >= 3 else (1., 1.)
        timestamp = struct.unpack_from("<Q", packet, 1296)[0] if version >= 4 else None
        session, channel, gain, fades = 0, 0, 1., []
        loop_start, loop_end = 0., 0.
        if version == 5:
            session, channel, gain, count = struct.unpack_from("<QifI", packet, 1304)
            if not session or not -1 <= channel < 64 or not math.isfinite(gain) or not 0 <= gain <= 16 or count > 8:
                raise ValueError("invalid song channel/session/gain/fade count")
            previous = 0
            for i in range(count):
                at, volume = struct.unpack_from("<Qf", packet, 1324 + i * 12)
                if not previous < at <= 86_400_000_000_000 or not math.isfinite(volume) or not 0 <= volume <= 16:
                    raise ValueError("invalid fade point")
                fades.append(dict(offset_ns=at, gain=volume))
                previous = at
            loop_start, loop_end = struct.unpack_from("<dd", packet, 1420)
            if flags & 4 and (not all(map(math.isfinite, (loop_start, loop_end))) or
                              not 0 <= loop_start < loop_end <= 86400):
                raise ValueError("invalid loop bounds")
        if (not all(map(math.isfinite, (position, rate, offset, *volumes))) or
                not -600 <= position < 86400 or not .25 <= rate <= 4 or
                not all(0 <= v <= 1 for v in volumes)):
            raise ValueError("invalid song position/rate/volume")
        text = lambda value: value.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return "song", dict(version=version, flags=flags, epoch=epoch, attempt=attempt,
                            position=position, rate=rate, offset=offset, status=text(status),
                            level=text(level), song=text(song), song_path_present=bool(text(path)),
                            obs_music_volume=volumes[0], obs_effects_volume=volumes[1],
                            timestamp_ns=timestamp, session_id=session, channel_id=channel,
                            trigger_gain=gain, fades=fades, loop_start=loop_start, loop_end=loop_end)
    if magic in (b"GDSFX1\0\0", b"GDSFX2\0\0"):
        modern = magic == b"GDSFX2\0\0"
        size = 44 if modern else 36
        if len(packet) < size:
            raise ValueError("truncated effects packet")
        _, version, sequence, rate, frames, timestamp, stream = struct.unpack_from("<8sIIIIQI", packet)
        session = struct.unpack_from("<Q", packet, 36)[0] if modern else 0
        if (version != (2 if modern else 1) or not 1 <= frames <= 512 or
                len(packet) != size + frames * 8 or not 8000 <= rate <= 192000 or
                stream > 1 or (modern and not session)):
            raise ValueError("invalid effects header/length")
        samples = struct.unpack_from(f"<{frames * 2}f", packet, size)
        if not all(map(math.isfinite, samples)):
            raise ValueError("nonfinite effects sample")
        return "effects", dict(version=version, sequence=sequence, rate=rate, frames=frames,
                               timestamp_ns=timestamp, stream=stream, session_id=session, samples=samples)
    if magic == b"GDCLK1\0\0":
        if len(packet) != CLOCK.size:
            raise ValueError("invalid clock packet length")
        _, version, kind, session, t1, t2, t3 = CLOCK.unpack(packet)
        if version != 1 or kind not in (1, 2) or not session or not t1:
            raise ValueError("invalid clock packet")
        if (kind == 1 and (t2 or t3)) or (kind == 2 and (not t2 or t3 < t2)):
            raise ValueError("invalid clock timestamps")
        return "clock", dict(kind=kind, session_id=session, t1=t1, t2=t2, t3=t3)
    raise ValueError("unknown packet magic")


class RelayRoutes:
    def __init__(self, destination):
        self.destination = destination
        self.session = None
        self.bridge = None
        self.probes = set()

    def route(self, kind, fields, peer):
        session = fields.get("session_id")
        if kind == "song" and fields["version"] == 5:
            if peer == self.destination:
                return None
            if session != self.session:
                self.session, self.bridge = session, peer
                self.probes.clear()
            if peer != self.bridge:
                return None
        if kind == "clock":
            if session != self.session:
                return None
            key = fields["t1"]
            if fields["kind"] == 1 and peer == self.destination and self.bridge:
                if len(self.probes) >= 64:
                    self.probes.remove(min(self.probes))
                self.probes.add(key)
                return self.bridge
            if fields["kind"] == 2 and peer == self.bridge and key in self.probes:
                self.probes.remove(key)
                return self.destination
            return None
        if peer == self.destination or (kind == "effects" and fields["version"] == 2 and session != self.session):
            return None
        return self.destination
