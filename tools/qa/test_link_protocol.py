import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

from link_protocol import CLOCK, SONG_TAG, RelayRoutes, SONG, now_ns, parse_packet, sign, tag

KEY = bytes(range(32))


def song(version=7):
    data = SONG.pack(b"GDSONG1\0", version, 1, 2, 3, 1.25, 1., 0., b"playing", b"level", b"song", b"private/path.ogg")
    if version >= 3:
        data += struct.pack("<ff", .8, .6)
    if version >= 4:
        data += struct.pack("<Q", now_ns())
    if version >= 5:
        data += struct.pack("<QifI", 1234, 2, .5, 1) + struct.pack("<Qf", 1000000, .25) + bytes(7 * 12) + struct.pack("<dd", 0., 0.)
    if version == 7:
        data += struct.pack("<BBBB", 2, 0, 0, 0)
        data += struct.pack("<fHHQ", .75, 1024, 0, 41) + struct.pack("<fHHQ", 1.5, 2048, 0, 42) + bytes(6 * 16)
        data += bytes(8 * 16)
        data = sign(KEY, data + bytes(16), 1696)
    elif version == 6:
        data = sign(KEY, data + bytes(16), 1436)
    return data


def effect(version=3):
    data = struct.pack("<8sIIIIQI", f"GDSFX{version}\0\0".encode(), version, 1, 48000, 1, now_ns(), 0)
    if version >= 2:
        data += struct.pack("<Q", 1234)
    if version == 3:
        data += bytes(16)
    return data + struct.pack("<ff", .25, -.25)


def clock(kind, t1, t2=0, t3=0):
    return CLOCK.pack(b"GDCLK1\0\0", 2, kind, 1234, t1, t2, t3) + bytes(16)


class PacketTests(unittest.TestCase):
    def test_song_versions(self):
        for version, size in ((2, 1288), (3, 1296), (4, 1304), (5, 1436), (6, 1452), (7, 1712)):
            with self.subTest(version=version):
                data = song(version)
                self.assertEqual(len(data), size)
                self.assertEqual(parse_packet(data)[1]["version"], version)
                for wrong in (data[:-1], data + b"\0"):
                    with self.assertRaises(ValueError):
                        parse_packet(wrong)
        self.assertEqual(parse_packet(song())[1]["fades"], [dict(offset_ns=1000000, gain=.25)])
        fields = parse_packet(song())[1]
        self.assertEqual(fields["stages"], [dict(pitch=.75, fft=1024, instance=41),
                                            dict(pitch=1.5, fft=2048, instance=42)])
        self.assertFalse(fields["group_excluded"])

    def test_v7_stage_rules(self):
        def reject(mutate):
            data = bytearray(song(7))
            mutate(data)
            with self.assertRaises(ValueError):
                parse_packet(data)

        for offset, fmt, value in ((1436, "B", 9), (1437, "B", 9), (1438, "B", 1), (1439, "B", 1),
                                   (1440, "f", float("nan")), (1440, "f", .4), (1440, "f", 2.1),
                                   (1444, "H", 0), (1444, "H", 768), (1446, "H", 1), (1448, "Q", 0),
                                   (1568, "f", .75), (12, "I", 16)):
            with self.subTest(offset=offset, value=value):
                reject(lambda data, o=offset, f=fmt, v=value: struct.pack_into("<" + f, data, o, v))
        reject(lambda data: struct.pack_into("<I", data, 12, 8))
        reject(lambda data: struct.pack_into("<B", data, 1437, 1))
        data = bytearray(song(7))
        struct.pack_into("<I", data, 12, 8)
        struct.pack_into("<B", data, 1436, 0)
        struct.pack_into("<128s", data, 1440, bytes(128))
        self.assertTrue(parse_packet(data)[1]["group_excluded"])
        # The global packet may carry effects stages; the same list on a voice packet is invalid.
        struct.pack_into("<I", data, 12, 1)
        struct.pack_into("<i", data, 1312, -1)
        struct.pack_into("<B", data, 1436, 1)
        struct.pack_into("<B", data, 1437, 1)
        struct.pack_into("<fHHQ", data, 1440, .75, 1024, 0, 41)
        struct.pack_into("<16s", data, 1456, bytes(16))
        struct.pack_into("<fHHQ", data, 1568, .5, 4096, 0, 43)
        assert parse_packet(data)[1]["effects_stages"] == [dict(pitch=.5, fft=4096, instance=43)]

    def test_song_bounds(self):
        for offset, fmt, value in ((1304, "Q", 0), (1312, "i", -2), (1312, "i", 64),
                                   (1316, "f", float("nan")), (1316, "f", 17),
                                   (1320, "I", 9), (1332, "f", -1),
                                   (24, "d", 86400), (32, "d", -.1), (32, "d", 16.5), (1288, "f", 2)):
            data = bytearray(song())
            struct.pack_into("<" + fmt, data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                parse_packet(data)
        data = bytearray(song())
        struct.pack_into("<i", data, 1312, -1)
        self.assertEqual(parse_packet(data)[1]["channel_id"], -1)

    def test_fade_times_need_only_be_nondecreasing(self):
        data = bytearray(song())
        struct.pack_into("<I", data, 1320, 3)
        struct.pack_into("<Qf", data, 1324, 0, .25)
        struct.pack_into("<Qf", data, 1336, 1_000_000, .5)
        struct.pack_into("<Qf", data, 1348, 1_000_000, 0.)
        fades = parse_packet(data)[1]["fades"]
        self.assertEqual([f["offset_ns"] for f in fades], [0, 1_000_000, 1_000_000])
        struct.pack_into("<Qf", data, 1348, 999_998, 0.)
        with self.assertRaises(ValueError):
            parse_packet(data)

    def test_loop_bounds(self):
        data = bytearray(song())
        struct.pack_into("<I", data, 12, 5)
        for start, end in ((0., .001), (5., 20.), (0., 86400.)):
            struct.pack_into("<dd", data, 1420, start, end)
            self.assertEqual(parse_packet(data)[1]["loop_end"], end)
        for start, end in ((0., 0.), (-1., 2.), (2., 1.), (0., 86401.), (float("nan"), 2.), (0., float("inf"))):
            struct.pack_into("<dd", data, 1420, start, end)
            with self.subTest(start=start, end=end), self.assertRaises(ValueError):
                parse_packet(data)

    def test_effects_versions_and_bounds(self):
        for version in (1, 2, 3):
            data = effect(version)
            self.assertEqual(parse_packet(data)[1]["samples"], (.25, -.25))
            for wrong in (data[:-1], data + b"\0", data[:35]):
                with self.assertRaises(ValueError):
                    parse_packet(wrong)
        for offset, fmt, value in ((16, "I", 7000), (20, "I", 0),
                                   (32, "I", 2), (36, "Q", 0), (60, "f", float("inf"))):
            data = bytearray(effect())
            struct.pack_into("<" + fmt, data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse_packet(data)

    def test_routes_do_not_learn_tap(self):
        obs, bridge, tap = ("127.0.0.1", 30001), ("127.0.0.1", 30002), ("127.0.0.1", 30003)
        route = RelayRoutes(obs)
        kind, fields = parse_packet(song())
        self.assertEqual(route.route(kind, fields, bridge), obs)
        self.assertEqual(route.route(*parse_packet(effect()), tap), obs)
        probe = clock(1, 20)
        reply = clock(2, 20, 30, 31)
        self.assertEqual(route.route(*parse_packet(probe), obs), bridge)
        self.assertIsNone(route.route(*parse_packet(reply), tap))
        self.assertEqual(route.route(*parse_packet(reply), bridge), obs)
        self.assertIsNone(route.route(*parse_packet(reply), bridge))

    def test_clock_validation(self):
        valid = clock(2, 20, 30, 31)
        self.assertEqual(parse_packet(valid)[1]["t3"], 31)
        self.assertEqual(parse_packet(CLOCK.pack(b"GDCLK1\0\0", 1, 2, 1234, 20, 30, 31))[1]["tag"], None)
        for packet in (valid[:-1], valid + b"\0", clock(2, 20, 31, 30)):
            with self.assertRaises(ValueError):
                parse_packet(packet)

    def test_real_loopback_handshake(self):
        root = Path(__file__).parent
        with tempfile.TemporaryDirectory() as directory, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as obs, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as bridge, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as tap:
            obs.bind(("127.0.0.1", 0))
            bridge.bind(("127.0.0.1", 0))
            tap.bind(("127.0.0.1", 0))
            obs.settimeout(2)
            bridge.settimeout(2)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
                reservation.bind(("127.0.0.1", 0))
                listen = reservation.getsockname()
            output = Path(directory) / "result.json"
            key_file = Path(directory) / "link-key"
            key_file.write_text(KEY.hex() + "\n")
            proc = subprocess.Popen([sys.executable, str(root / "live-effects-relay.py"), "--seconds", "1", "--listen-port", str(listen[1]), "--destination-port", str(obs.getsockname()[1]), "--output", str(output), "--key-file", str(key_file)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                self.assertIn("Relaying", proc.stdout.readline())
                original = song()
                bridge.sendto(original, listen)
                received, sender = obs.recvfrom(8192)
                self.assertEqual(received[264:1288], bytes(1024))
                self.assertEqual(received[:264], original[:264])
                self.assertEqual(received[1288:SONG_TAG], original[1288:SONG_TAG])
                self.assertEqual(received[SONG_TAG:], tag(KEY, received, SONG_TAG))
                tap.sendto(effect(), listen)
                self.assertEqual(parse_packet(obs.recvfrom(8192)[0])[0], "effects")
                probe = clock(1, now_ns())
                obs.sendto(probe, sender)
                received, source = bridge.recvfrom(8192)
                self.assertEqual(received, probe)
                _, fields = parse_packet(received)
                reply = clock(2, fields["t1"], now_ns(), now_ns())
                bridge.sendto(reply, source)
                self.assertEqual(obs.recvfrom(8192)[0], reply)
                stdout, stderr = proc.communicate(timeout=4)
                self.assertEqual(proc.returncode, 0, stdout + stderr)
                result = json.loads(output.read_text())
                self.assertTrue(result["pass_relay"])
                self.assertEqual(result["clock_replies_forwarded"], 1)
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate()

    def test_no_traffic_is_failure(self):
        with tempfile.TemporaryDirectory() as directory, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
            reservation.close()
            output = Path(directory) / "empty.json"
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("game-link-check.py")), "--seconds", ".1", "--port", str(port), "--output", str(output)], capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(json.loads(output.read_text())["pass_valid_song_heartbeat"])


if __name__ == "__main__":
    unittest.main()
