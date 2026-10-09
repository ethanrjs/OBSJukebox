#include "../../src/LinkPacket.hpp"
#include <cstddef>
#include <cstring>
#include <iostream>
#include <iomanip>
#define FIELD(T, F) std::cout << "field " #T " " #F " " << offsetof(T, F) << " " << sizeof(((T*)nullptr)->F) << "\n";
template<class T> void fixture(const char* name, const T& packet, size_t size=sizeof(T)) {
 std::cout << "packet " << name << " ";
 auto bytes=reinterpret_cast<const unsigned char*>(&packet);
 for(size_t i=0;i<size;++i) std::cout << std::hex << std::setfill('0') << std::setw(2) << unsigned(bytes[i]);
 std::cout << std::dec << "\n";
}
int main() {
 std::cout << "size SongFadePoint " << sizeof(SongFadePoint) << "\n";
 FIELD(SongFadePoint, offsetNs)
 FIELD(SongFadePoint, gain)
 std::cout << "size SongLinkPacket " << sizeof(SongLinkPacket) << "\n";
 FIELD(SongLinkPacket, magic)
 FIELD(SongLinkPacket, version)
 FIELD(SongLinkPacket, flags)
 FIELD(SongLinkPacket, epoch)
 FIELD(SongLinkPacket, attempt)
 FIELD(SongLinkPacket, position)
 FIELD(SongLinkPacket, rate)
 FIELD(SongLinkPacket, offset)
 FIELD(SongLinkPacket, status)
 FIELD(SongLinkPacket, level)
 FIELD(SongLinkPacket, song)
 FIELD(SongLinkPacket, path)
 FIELD(SongLinkPacket, musicVolume)
 FIELD(SongLinkPacket, effectsVolume)
 FIELD(SongLinkPacket, timestamp)
 FIELD(SongLinkPacket, sessionID)
 FIELD(SongLinkPacket, channelID)
 FIELD(SongLinkPacket, triggerGain)
 FIELD(SongLinkPacket, fadeCount)
 FIELD(SongLinkPacket, fades)
 FIELD(SongLinkPacket, loopStart)
 FIELD(SongLinkPacket, loopEnd)
 std::cout << "size ClockSyncPacket " << sizeof(ClockSyncPacket) << "\n";
 FIELD(ClockSyncPacket, magic)
 FIELD(ClockSyncPacket, version)
 FIELD(ClockSyncPacket, kind)
 FIELD(ClockSyncPacket, sessionID)
 FIELD(ClockSyncPacket, t1)
 FIELD(ClockSyncPacket, t2)
 FIELD(ClockSyncPacket, t3)
 std::cout << "size EffectsPacket " << sizeof(EffectsPacket) << "\n";
 FIELD(EffectsPacket, magic)
 FIELD(EffectsPacket, version)
 FIELD(EffectsPacket, sequence)
 FIELD(EffectsPacket, sampleRate)
 FIELD(EffectsPacket, frames)
 FIELD(EffectsPacket, timestamp)
 FIELD(EffectsPacket, stream)
 FIELD(EffectsPacket, samples)
 std::cout << "size EffectsPacketV2 " << sizeof(EffectsPacketV2) << "\n";
 FIELD(EffectsPacketV2, magic)
 FIELD(EffectsPacketV2, version)
 FIELD(EffectsPacketV2, sequence)
 FIELD(EffectsPacketV2, sampleRate)
 FIELD(EffectsPacketV2, frames)
 FIELD(EffectsPacketV2, timestamp)
 FIELD(EffectsPacketV2, stream)
 FIELD(EffectsPacketV2, sessionID)
 FIELD(EffectsPacketV2, samples)
 SongLinkPacket song; song.flags=4; song.epoch=17; song.attempt=-3; song.position=1.25; song.rate=1.5; song.offset=-2.5;
 std::memcpy(song.status,"playing",sizeof("playing")); std::memcpy(song.level,"fixture level",sizeof("fixture level")); std::memcpy(song.song,"fixture song",sizeof("fixture song")); std::memcpy(song.path,"fixture.ogg",sizeof("fixture.ogg"));
 song.musicVolume=.25f; song.effectsVolume=.5f; song.timestamp=123456; song.sessionID=765432; song.channelID=7; song.triggerGain=2.f;
 song.fadeCount=8; for(unsigned i=0;i<8;++i) { song.fades[i].offsetNs=(i+1)*100; song.fades[i].gain=float(i+1)/2; }
 song.loopStart=3.25; song.loopEnd=9.5;
 fixture("song5",song); song.version=4; fixture("song4",song,SONG_LINK_V4_SIZE); song.version=3; fixture("song3",song,SONG_LINK_V3_SIZE); song.version=2; fixture("song2",song,SONG_LINK_V2_SIZE);
 ClockSyncPacket clock; clock.kind=2; clock.sessionID=765432; clock.t1=11; clock.t2=22; clock.t3=33; fixture("clock",clock);
 EffectsPacket effects; effects.sequence=19; effects.sampleRate=48000; effects.frames=512; effects.timestamp=123456; effects.stream=1;
 for(unsigned i=0;i<1024;++i) effects.samples[i]=float(i)/1024; fixture("effects1",effects);
 EffectsPacketV2 modern; modern.sequence=19; modern.sampleRate=48000; modern.frames=512; modern.timestamp=123456; modern.stream=1; modern.sessionID=765432;
 for(unsigned i=0;i<1024;++i) modern.samples[i]=float(i)/1024; fixture("effects2",modern);
}
