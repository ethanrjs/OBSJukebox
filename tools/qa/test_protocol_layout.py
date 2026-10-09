"""Compare native wire layouts and serialized fixtures with the Python receiver."""
import struct
import subprocess
import sys
from link_protocol import parse_packet, SONG, CLOCK, SONG_SIZES

schemas = {
    'SongFadePoint': [('offsetNs', 'Q'), ('gain', 'f')],
    'SongLinkPacket': list(zip('magic version flags epoch attempt position rate offset status level song path musicVolume effectsVolume timestamp sessionID channelID triggerGain fadeCount fades loopStart loopEnd'.split(),
                               ['8s','I','I','I','i','d','d','d','24s','96s','96s','1024s','f','f','Q','Q','i','f','I','96s','d','d'])),
    'ClockSyncPacket': list(zip('magic version kind sessionID t1 t2 t3'.split(), ['8s','I','I','Q','Q','Q','Q'])),
}
for name, modern in [('EffectsPacket', False), ('EffectsPacketV2', True)]:
    schemas[name] = list(zip('magic version sequence sampleRate frames timestamp stream'.split(), ['8s','I','I','I','I','Q','I']))
    if modern:
        schemas[name].append(('sessionID','Q'))
    schemas[name].append(('samples','1024f'))
expected = {}
for name, fields in schemas.items():
    offset = 0
    for field, fmt in fields:
        size = struct.calcsize('<' + fmt)
        expected['field', name, field] = (offset, size)
        offset += size
    expected['size', name] = (offset,)
packets = {}
for line in subprocess.check_output([sys.argv[1]], text=True).splitlines():
    parts = line.split()
    if parts[0] == 'packet':
        packets[parts[1]] = bytes.fromhex(parts[2])
    else:
        count = 3 if parts[0] == 'field' else 2
        key = tuple(parts[:count])
        actual = tuple(map(int, parts[count:]))
        assert actual == expected.pop(key), (key, actual)
assert not expected, expected
assert SONG.size == 1288 and CLOCK.size == 48
for version in range(2, 6):
    packet = packets.pop('song' + str(version))
    assert len(packet) == SONG_SIZES[version]
    kind, fields = parse_packet(packet)
    assert kind == 'song'
    baseline = dict(version=version, flags=4, epoch=17, attempt=-3, position=1.25, rate=1.5, offset=-2.5,
                    status='playing', level='fixture level', song='fixture song', song_path_present=True,
                    obs_music_volume=.25 if version >= 3 else 1., obs_effects_volume=.5 if version >= 3 else 1.,
                    timestamp_ns=123456 if version >= 4 else None, session_id=765432 if version == 5 else 0,
                    channel_id=7 if version == 5 else 0, trigger_gain=2. if version == 5 else 1.,
                    fades=[dict(offset_ns=(i+1)*100, gain=(i+1)/2) for i in range(8)] if version == 5 else [],
                    loop_start=3.25 if version == 5 else 0., loop_end=9.5 if version == 5 else 0.)
    assert fields == baseline, (version, fields, baseline)
kind, fields = parse_packet(packets.pop('clock'))
assert (kind, fields) == ('clock', dict(kind=2, session_id=765432, t1=11, t2=22, t3=33))
for version in (1, 2):
    kind, fields = parse_packet(packets.pop('effects' + str(version)))
    assert kind == 'effects'
    assert fields == dict(version=version, sequence=19, rate=48000, frames=512, timestamp_ns=123456,
                          stream=1, session_id=765432 if version == 2 else 0,
                          samples=tuple(i/1024 for i in range(1024)))
assert not packets
print('Native/Python protocol layout and all serialized fields agree')
