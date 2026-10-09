#include "../../../obs-plugin/separate-song.cpp"
#include <iostream>
#include <fstream>
#include <limits>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
#include "../../../src/MonotonicClock.hpp"
#include "../../../src/Bridge.hpp"
#include "../../../src/TapClock.hpp"

static SongLinkPacket published;
static int captureSend(SongSocket,const char* data,int size,int,const sockaddr*,
#ifdef _WIN32
    int
#else
    socklen_t
#endif
) {
    if(size==sizeof(published))std::memcpy(&published,data,size);
    return size;
}
#define sendto captureSend
#include "../../../src/Bridge.cpp"
#undef sendto

static int failures=0;
static void check(bool ok,const char* message) {
    std::cout<<(ok?"PASS: ":"FAIL: ")<<message<<'\n';
    if(!ok)++failures;
}
static void writeRamp(const char* path,bool negative=false) {
    std::ofstream out(path,std::ios::binary);
    auto word=[&](uint32_t n,int bytes){for(int i=0;i<bytes;++i)out.put(char(n>>(i*8)));};
    constexpr uint32_t frames=48000*4,bytes=frames*4;
    out.write("RIFF",4);word(36+bytes,4);out.write("WAVEfmt ",8);word(16,4);
    word(1,2);word(2,2);word(48000,4);word(48000*4,4);word(4,2);word(16,2);
    out.write("data",4);word(bytes,4);
    for(unsigned i=0;i<frames;++i){auto sample=int16_t(32768*(.1+double(i)/480000)*(negative?-1:1));word(uint16_t(sample),2);word(uint16_t(sample),2);}
}
static void sendState(SongSocket socket,SongLinkPacket p,uint64_t capture,int version=4) {
    p.version=version;
    p.timestamp=capture;
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    auto bytes=version==2?SONG_LINK_V2_SIZE:version==3?SONG_LINK_V3_SIZE:version==4?SONG_LINK_V4_SIZE:sizeof(p);
    auto sent=sendto(socket,reinterpret_cast<const char*>(&p),int(bytes),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    if(sent!=bytes)check(false,"loopback send succeeds");
}
static std::vector<Captured> output() {std::lock_guard lock(capturedMutex);return captured;}
static void resetOutput(){std::lock_guard lock(capturedMutex);captured.clear();}
static void pauseTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    sendState(sender,p,os_gettime_ns(),3);
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        bool heard=false;for(const auto& b:output())for(float s:b.samples)heard|=s>.1f;
        check(heard,"legacy v3 music starts");
        auto pauseAt=os_gettime_ns();p.flags=1;p.position=1.12;sendState(sender,p,pauseAt,3);
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        uint64_t firstSilent=0;
        for(const auto& b:output())if(b.wall>pauseAt && !firstSilent)for(size_t i=0;i<b.samples.size()/2;++i){
            if(b.samples[i*2]==0){firstSilent=b.timestamp+i*1000000000/48000;break;}
        }
        check(firstSilent>=pauseAt-2000000 && firstSilent<=pauseAt+15000000,"pause takes effect on the audio timeline");
    }
    receiver.reset();closeSocket(sender);
}
static void equalTimestampStopTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    auto stamp=os_gettime_ns();sendState(sender,p,stamp);
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto stoppedQuickly=[&]{
            bool heard=false;
            for(const auto& b:output())if(b.timestamp>=stamp+10000000)
                for(float sample:b.samples)heard|=sample>.1f;
            auto sent=os_gettime_ns();sendState(sender,p,stamp);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            bool silent=true;unsigned blocks=0;
            for(const auto& b:output())if(b.wall>=sent+70000000){
                ++blocks;for(float sample:b.samples)silent&=sample==0;
            }
            auto state=receiver->playbackWindow(stamp,stamp+1).front();
            return heard && silent && blocks>0 && state.flags==p.flags && state.epoch==p.epoch;
        };
        p.flags=1;
        check(stoppedQuickly(),"equal-timestamp pause silences music without waiting for staleness");
        p.flags=3;stamp=os_gettime_ns();sendState(sender,p,stamp);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        p.flags=0;++p.epoch;p.path[0]=0;
        check(stoppedQuickly(),"equal-timestamp stop with a new epoch replaces the playing state");
    }
    receiver.reset();closeSocket(sender);
}
static void delayedPositionTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    auto capture=os_gettime_ns();std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sendState(sender,p,capture);
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        bool checked=false,correct=true;
        for(const auto& b:output())if(b.timestamp>capture+120000000 && b.timestamp<capture+190000000){
            auto seconds=1+double(b.timestamp-capture)/1e9;
            correct&=std::abs(b.samples[0]-(.1+seconds*.1))<.0005;
            checked=true;
        }
        check(checked && correct,"100 ms packet delay does not shift the song position");
    }
    receiver.reset();closeSocket(sender);
}
static void transitionTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    auto base=os_gettime_ns()-10000000; // keeps the last event under the receiver's 100 ms future limit
    // crossfades: whether the event starts a declick crossfade, whose frames are not compared
    struct Event {uint64_t at;SongLinkPacket p;bool crossfades;};
    std::vector<Event> events;
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    auto add=[&](int milliseconds,bool crossfades=true){
        auto at=uint64_t(int64_t(base)+int64_t(milliseconds)*1000000);events.push_back({at,p,crossfades});sendState(sender,p,at);
    };
    add(-100);
    // Events sit 10 ms apart: a 5 ms crossfade, then 5 ms compared exactly. The first leaves the
    // source time to start before it.
    p.flags=1;add(35);                                             // pause
    p.flags=3;p.position=1.4;add(45);                              // resume
    p.flags=1;add(55);                                             // death
    p.flags=3;p.epoch=2;p.position=2;p.offset=.1;p.rate=2;p.musicVolume=.5;add(65); // restart
    p.epoch=3;p.position=2.035;add(75);                            // seek
    p.position=2.055;p.musicVolume=.25;add(85,false);              // gain change only
    p.position=2.075;
    std::strcpy(p.path,"artifacts/audio-tests/negative.wav");add(95); // song change
    p.flags=0;add(105);                                            // disable
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(230));
        constexpr uint64_t crossfadeNs=(declickFrames+2)*1000000000ULL/48000;
        bool correct=true;std::vector<unsigned> counts(events.size());
        auto fadedIn=std::max(base,output().front().timestamp)+crossfadeNs; // the source fades in when it starts
        for(const auto& block:output())for(size_t frame=0;frame<480;++frame){
            auto at=block.timestamp+frame*1000000000/48000;
            if(at<base)continue;
            size_t event=0;while(event+1<events.size() && events[event+1].at<=at)++event;
            const auto& current=events[event];
            ++counts[event];
            if(at<fadedIn || (current.crossfades && at<current.at+crossfadeNs))continue;
            double expected=0;
            if((current.p.flags&3)==3){
                auto position=current.p.position+current.p.offset+double(int64_t(at)-int64_t(current.at))/1e9*current.p.rate;
                expected=(.1+position*.1)*current.p.musicVolume*(std::strstr(current.p.path,"negative")?-1:1);
            }
            if(std::abs(block.samples[frame*2]-expected)>=.0002){
                if(correct)std::cerr<<"First mismatch: event "<<event<<", time "<<double(int64_t(at)-int64_t(base))/1e6<<" ms, expected "<<expected<<", actual "<<block.samples[frame*2]<<'\n';
                correct=false;
            }
        }
        check(correct && std::all_of(counts.begin(),counts.end(),[](unsigned n){return n>0;}),
            "pause, resume, death, restart, path and gain changes occur at the correct sample");
    }
    receiver.reset();closeSocket(sender);
}
// A hard cut drops a ~0.2 sample to silence (or jumps to another song position) in one frame,
// which OBS viewers hear as a click. Every transition should instead ramp over a few ms.
static void declickTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    auto base=os_gettime_ns();
    auto at=[&](int milliseconds){return uint64_t(int64_t(base)+int64_t(milliseconds)*1000000);};
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    sendState(sender,p,at(0));
    p.flags=1;sendState(sender,p,at(20));                        // pause
    p.flags=3;p.position=1.5;sendState(sender,p,at(40));         // resume
    p.epoch=2;p.position=.2;sendState(sender,p,at(60));          // restart
    p.position=.24;std::strcpy(p.path,"artifacts/audio-tests/negative.wav");sendState(sender,p,at(80)); // song change
    p.flags=0;sendState(sender,p,at(95));                        // OBS Jukebox disabled
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        float previous=0,largestStep=0;bool silentWhilePaused=false,negativeAfterChange=false;
        for(const auto& block:output())for(size_t frame=0;frame<480;++frame){
            float sample=block.samples[frame*2];
            largestStep=std::max(largestStep,std::abs(sample-previous));previous=sample;
            auto time=block.timestamp+frame*1000000000ULL/48000;
            silentWhilePaused|=time>at(28) && time<at(36) && sample==0;
            negativeAfterChange|=time>at(87) && time<at(93) && sample<-.1f;
        }
        check(silentWhilePaused && negativeAfterChange && largestStep<.005f,
            "start, pause, resume, restart, song change and disable ramp instead of clicking");
    }
    receiver.reset();closeSocket(sender);
}
// A stale packet only fades out a voice that was playing. The source drops idle voices, so one
// that is recreated for a stale packet must not open the song again every block.
static void staleVoiceTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    sendState(sender,p,os_gettime_ns()-600000000);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto opens=Decoder::testOpens.load();
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    check(Decoder::testOpens.load()==opens,"a stale packet does not reopen the song for a dropped voice");
    receiver.reset();closeSocket(sender);
}
static void effectsTransitionTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    sendState(sender,SongLinkPacket{},os_gettime_ns()-200000000);
    EffectsPacket initial;initial.frames=1;initial.sampleRate=48000;initial.timestamp=os_gettime_ns();
    sendto(sender,reinterpret_cast<const char*>(&initial),36+initial.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto base=os_gettime_ns();
    SongLinkPacket p;p.flags=1;p.effectsVolume=.2;sendState(sender,p,base-100000000);
    p.flags=0;sendState(sender,p,base+30000000);
    p.flags=1;p.effectsVolume=.8;sendState(sender,p,base+60000000);
    for(int i=-5;i<12;++i){
        EffectsPacket e;e.frames=480;e.sampleRate=48000;e.timestamp=uint64_t(int64_t(base)+i*10000000LL);
        std::fill_n(e.samples,960,.25f);
        sendto(sender,reinterpret_cast<const char*>(&e),36+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        bool correct=true;unsigned count=0;
        for(const auto& b:output())for(size_t i=0;i<480;++i){
            auto at=b.timestamp+i*1000000000/48000;
            if(at<base || at>base+100000000)continue;
            auto expected=at<base+30000000?.05:at<base+60000000?0:.2;
            correct&=std::abs(b.samples[i*2]-expected)<.00001;++count;
        }
        check(correct && count>4000,"legacy effects gain changes follow the timeline after fallback clock initialization");
    }
    receiver.reset();closeSocket(sender);
}
// SFX audio comes from FMOD's mixer thread and keeps arriving while GD's main thread stalls,
// so a stale status packet must not mute it.
static void staleStatusEffectsTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    EffectsPacket initial;initial.frames=1;initial.sampleRate=48000;initial.timestamp=os_gettime_ns();
    sendto(sender,reinterpret_cast<const char*>(&initial),36+initial.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto base=os_gettime_ns();
    SongLinkPacket p;p.flags=1;sendState(sender,p,base-600000000);
    for(int i=0;i<12;++i){
        EffectsPacket e;e.frames=480;e.sampleRate=48000;e.timestamp=base+i*10000000ULL;
        std::fill_n(e.samples,960,.25f);
        sendto(sender,reinterpret_cast<const char*>(&e),36+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        bool correct=true;unsigned count=0;
        for(const auto& b:output())for(size_t i=0;i<480;++i){
            auto at=b.timestamp+i*1000000000/48000;
            if(at<base+10000000 || at>base+90000000)continue;
            correct&=std::abs(b.samples[i*2]-.25f)<.00001;++count;
        }
        check(correct && count>3000,"effects keep playing when the latest status packet is over 400 ms old");
    }
    receiver.reset();closeSocket(sender);
}
// One probe delayed on its way back can suggest an offset several ms off; it must neither move
// the clock nor clear buffered audio. A real clock jump (suspend) still recalibrates.
static void clockNoiseTest() {
    LinkClock clock;bool changed=false;
    auto probe=[&](uint64_t t1,uint64_t t2,uint64_t t3,uint64_t t4){
        ClockSyncPacket p;p.t1=t1;p.t2=t2;p.t3=t3;return clock.observe(p,t4,changed);
    };
    uint64_t t=1000000000;
    probe(t,t+50000,t+50000,t+100000);                       // 0.1 ms round trip, offset 0
    t+=6000000000;
    probe(t,t+50000,t+50000,t+20100000);                     // reply delayed 20 ms
    uint64_t sample=t;clock.translate(sample);
    check(!changed && sample==t,"a delayed clock probe neither shifts the offset nor clears audio");
    t+=1000000000;
    probe(t,t+3600000000000ULL,t+3600000000000ULL,t+100000); // sender clock jumped an hour
    check(changed,"a clock jump still recalibrates");
}
static void clockSlewTest() {
    LinkClock clock;bool changed=false;
    auto probe=[&](uint64_t sent,uint64_t delay){
        ClockSyncPacket p;p.t1=sent;p.t2=sent+50000;p.t3=p.t2;
        return clock.observe(p,sent+delay,changed);
    };
    uint64_t t=1000000000;
    probe(t,100000);
    t+=250000000;
    uint64_t before=t+50000;clock.translate(before);
    probe(t,300000); // accepted 0.3 ms probe suggests a 0.1 ms offset change
    uint64_t after=t+50000;clock.translate(after);
    check(after==before && !changed,"accepted clock corrections start without a timestamp jump");
    uint64_t later=t+100050000;clock.translate(later);
    auto correction=later-(t+100050000);
    check(correction>0 && correction<=10000,"clock corrections slew at no more than 100 ppm");
    // A correction in the opposite direction must also remain continuous and monotonic.
    ClockSyncPacket p;p.t1=t+250000000;p.t2=p.t1+250000;p.t3=p.t2;
    uint64_t oldTime=p.t3;clock.translate(oldTime);
    clock.observe(p,p.t1+300000,changed);
    uint64_t newTime=p.t3;clock.translate(newTime);
    uint64_t next=p.t3+10000000;clock.translate(next);
    check(newTime==oldTime && next>newTime && next-newTime>=9999000,
        "negative clock corrections remain continuous and preserve packet order");
}
static void protocolTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);
    SongLinkPacket p;p.flags=3;p.position=1;
    sendState(sender,p,0,2);std::this_thread::sleep_for(std::chrono::milliseconds(15));
    auto v2=receiver->get();
    check(v2.connected && v2.packet.version==2 && v2.packet.musicVolume==1 && v2.packet.effectsVolume==1,"v2 packets retain default gains");
    auto at=os_gettime_ns();sendState(sender,p,at);std::this_thread::sleep_for(std::chrono::milliseconds(15));
    p.epoch=99;sendState(sender,p,at-1000000);sendState(sender,p,at+2000000000);
    sendState(sender,p,0);sendState(sender,p,os_gettime_ns(),6);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    check(receiver->get().packet.timestamp==at && receiver->get().packet.epoch==0,"reordered, stale, future and unsupported packets cannot replace the latest state");
    auto before=receiver->playbackWindow(at-1,at+1);
    check(before.size()==2 && before.front().timestamp==at-1000000 && before.front().epoch==99 && before.back().timestamp==at,
        "reordered packets fill earlier timeline history without replacing a later transition");
    auto initial=receiver->playbackWindow(1,2);
    check(initial.size()==1 && initial.front().flags==0,"future state cannot start music before its timestamp");
    receiver.reset();closeSocket(sender);
}

struct SyncedSender {
    SongSocket socket=BAD_SOCKET;
    std::atomic<int64_t> clockOffset;
    uint64_t session;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> probes{0};
    std::thread replies;
    explicit SyncedSender(int64_t offset):clockOffset(offset),session(os_gettime_ns()) {
        socket=::socket(AF_INET,SOCK_DGRAM,0);
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(socket!=BAD_SOCKET && bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))==0,
            "foreign-clock sender binds loopback");
        nonblocking(socket);
        replies=std::thread([this]{
            while(!stop){
                ClockSyncPacket probe{};sockaddr_in peer{};
#ifdef _WIN32
                int peerSize=sizeof(peer);
#else
                socklen_t peerSize=sizeof(peer);
#endif
                auto n=recvfrom(socket,reinterpret_cast<char*>(&probe),sizeof(probe),0,
                    reinterpret_cast<sockaddr*>(&peer),&peerSize);
                auto received=stamp(os_gettime_ns());
                if(n==sizeof(probe) && !std::memcmp(probe.magic,"GDCLK1\0",8) &&
                    probe.version==1 && probe.kind==1 && probe.sessionID==session){
                    ++probes;
                    probe.kind=2;probe.t2=received;probe.t3=stamp(os_gettime_ns());
                    sendto(socket,reinterpret_cast<const char*>(&probe),sizeof(probe),0,
                        reinterpret_cast<sockaddr*>(&peer),peerSize);
                }else {
                    fd_set readable;FD_ZERO(&readable);FD_SET(socket,&readable);
                    timeval timeout{};timeout.tv_usec=10000;
                    select(int(socket)+1,&readable,nullptr,nullptr,&timeout);
                }
            }
        });
    }
    ~SyncedSender(){stop=true;if(replies.joinable())replies.join();closeSocket(socket);}
    uint64_t stamp(uint64_t native) const {return uint64_t(int64_t(native)+clockOffset.load());}
    void send(SongLinkPacket p,uint64_t native) {
        p.sessionID=session;sendState(socket,p,stamp(native),5);
    }
    bool synchronize(unsigned epoch=717) {
        SongLinkPacket hello;hello.epoch=epoch;hello.flags=0;
        for(int i=0;i<60;++i){
            send(hello,os_gettime_ns());std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto current=receiver->get();
            if(current.connected && current.packet.sessionID==session && current.packet.epoch==epoch &&
                std::abs(double(int64_t(current.packet.timestamp)-int64_t(os_gettime_ns())))<100000000)
                return true;
        }
        auto current=receiver->get();
        std::cerr<<"Clock handshake failed: probes="<<probes<<", connected="<<current.connected
            <<", session="<<current.packet.sessionID<<" expected="<<session<<", stampDelta="
            <<int64_t(current.packet.timestamp)-int64_t(os_gettime_ns())<<'\n';
        return false;
    }
};

// GD's main thread can stall for most of a second (tearing down a level) while FMOD's mixer thread
// keeps playing the song and sending effects. Music then plays on past 400 ms. With the effects
// stream gone too (GD closed or crashed), or with the main thread still sending other packets
// (this voice's stop was lost), the voice still goes stale.
enum class Silence { MainThreadStall, Crash, VoiceOnly };
static void mainThreadStallTest(Silence silence) {
    receiver=std::make_unique<Receiver>();resetOutput();
    {
        SyncedSender sender(3600000000000LL);
        check(sender.synchronize(),"stall test synchronizes its sender");
        SongLinkPacket music;music.flags=7;music.position=1;music.loopStart=0;music.loopEnd=4;
        std::strcpy(music.path,"artifacts/audio-tests/ramp.wav");
        auto base=os_gettime_ns();sender.send(music,base);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto mappedBase=receiver->get().packet.timestamp;
        auto effectsSocket=socket(AF_INET,SOCK_DGRAM,0);
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            for(unsigned sequence=0;os_gettime_ns()<base+800000000;++sequence){
                auto now=os_gettime_ns();
                if(silence!=Silence::Crash){
                    EffectsPacketV2 p;p.sessionID=sender.session;p.frames=480;p.sampleRate=48000;p.sequence=sequence;
                    p.timestamp=sender.stamp(now);std::fill_n(p.samples,960,0.f);
                    sendto(effectsSocket,reinterpret_cast<const char*>(&p),44+p.frames*8,0,
                        reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
                }
                if(silence==Silence::VoiceOnly){
                    SongLinkPacket status;status.channelID=-1;status.flags=1;sender.send(status,now);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            unsigned audible=0,frames=0;
            for(const auto& b:output())for(unsigned i=0;i<480;++i){
                auto delta=int64_t(b.timestamp+i*1000000000ULL/48000)-int64_t(mappedBase);
                if(delta<500000000 || delta>750000000)continue;
                ++frames;audible+=b.samples[i*2]>.1f;
            }
            bool plays=silence==Silence::MainThreadStall;
            check(frames>10000 && (plays?audible==frames:audible==0),
                silence==Silence::MainThreadStall?"music plays through a main-thread stall while GD still mixes":
                silence==Silence::Crash?"music goes stale once GD stops mixing":
                "a voice the main thread stops refreshing still goes stale");
        }
        closeSocket(effectsSocket);
    }
    receiver.reset();
}
static void v5ClockCorrectedPauseTest() {
    receiver=std::make_unique<Receiver>();resetOutput();
    auto sender=socket(AF_INET,SOCK_DGRAM,0);nonblocking(sender);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    bool bound=sender!=BAD_SOCKET && bind(sender,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0;
    SongLinkPacket p;p.sessionID=os_gettime_ns();sendState(sender,p,os_gettime_ns(),5);
    auto reply=[&](uint64_t senderOffset,unsigned delayMs){
        auto limit=os_gettime_ns()+1000000000;
        while(os_gettime_ns()<limit){
            ClockSyncPacket probe{};
#ifdef _WIN32
            int length=sizeof(addr);
#else
            socklen_t length=sizeof(addr);
#endif
            auto bytes=recvfrom(sender,reinterpret_cast<char*>(&probe),sizeof(probe),0,
                reinterpret_cast<sockaddr*>(&addr),&length);
            if(bytes==sizeof(probe) && probe.sessionID==p.sessionID && probe.kind==1){
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
                probe.kind=2;probe.t2=probe.t3=probe.t1+senderOffset;
                return sendto(sender,reinterpret_cast<const char*>(&probe),sizeof(probe),0,
                    reinterpret_cast<sockaddr*>(&addr),length)==sizeof(probe);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    // Initial RTT is slow, then each reply gets faster, so both corrections are accepted.
    bool synced=bound && reply(15000000,30) && reply(20000000,10);
    check(synced,"v5 pause test exchanges clock probes");
    if(synced){
        auto stamp=os_gettime_ns();p.flags=3;p.position=1;p.epoch=718;
        std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");sendState(sender,p,stamp,5);
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto playing=receiver->get().packet;
            bool corrected=reply(20000000,0);
            auto pausedAt=os_gettime_ns();p.flags=1;sendState(sender,p,stamp,5);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            auto paused=receiver->get().packet;
            bool heard=false,silent=true;unsigned count=0;
            for(const auto& block:output()){
                if(block.wall<pausedAt)for(float sample:block.samples)heard|=sample>.1f;
                if(block.wall>pausedAt+70000000){
                    ++count;for(float sample:block.samples)silent&=sample==0;
                }
            }
            check(corrected && heard && silent && count && paused.flags==1 && paused.timestamp==playing.timestamp,
                "v5 pause replaces a repeated sender timestamp across a clock correction");
        }
    }
    receiver.reset();closeSocket(sender);
}

static void foreignClockTest(int64_t offset) {
    receiver=std::make_unique<Receiver>();resetOutput();
    {
        SyncedSender sender(offset);
        check(sender.synchronize(),offset>0?"v5 synchronizes a sender clock ahead of OBS":"v5 synchronizes a sender clock behind OBS");
        SongLinkPacket p;p.flags=3;p.position=1;p.epoch=718;
        std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
        auto capture=os_gettime_ns();std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sender.send(p,capture);
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            bool correct=true;unsigned samples=0;
            for(const auto& b:output())if(b.timestamp>capture+120000000 && b.timestamp<capture+190000000){
                auto expected=.1+(1+double(b.timestamp-capture)/1e9)*.1;
                correct&=std::abs(b.samples[0]-expected)<.0004;++samples;
            }
            check(samples>=4 && correct,"foreign clock plus 100 ms delivery delay preserves the sampled position");
        }
        auto accepted=receiver->get().packet;
        p.epoch=999;sender.send(p,capture-1000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        check(receiver->get().packet.epoch==accepted.epoch,"v5 reordered history cannot regress the latest state");
        sender.send(p,os_gettime_ns()+2000000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        check(receiver->get().packet.epoch==accepted.epoch,"v5 rejects future states after clock conversion");
    }
    receiver.reset();
}

static void multipleVoiceFadeTest() {
    receiver=std::make_unique<Receiver>();resetOutput();
    {
        SyncedSender sender(3600000000000LL);
        check(sender.synchronize(),"multi-voice test synchronizes its sender");
        auto base=os_gettime_ns();
        SongLinkPacket global;global.channelID=-1;global.flags=1;global.effectsVolume=.2f;
        sender.send(global,base-20000000);
        SongLinkPacket a;a.flags=3;a.channelID=0;a.position=1;a.musicVolume=.5f;a.triggerGain=.4f;
        std::strcpy(a.path,"artifacts/audio-tests/ramp.wav");
        sender.send(a,base);
        SongLinkPacket b=a;b.channelID=7;b.triggerGain=1;b.musicVolume=.25f;
        b.fadeCount=1;b.fades[0]={80000000,0};
        std::strcpy(b.path,"artifacts/audio-tests/negative.wav");
        sender.send(b,base);
        b.flags=1;sender.send(b,base+90000000);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            std::this_thread::sleep_for(std::chrono::milliseconds(210));
            bool mixed=true,survived=true;unsigned fadeFrames=0,remainingFrames=0;
            for(const auto& block:output())for(unsigned frame=0;frame<480;++frame){
                auto at=block.timestamp+frame*1000000000ULL/48000;
                auto elapsed=double(int64_t(at)-int64_t(base))/1e9;
                double ramp=.1+(1+elapsed)*.1;
                if(elapsed>.01 && elapsed<.07){
                    double expected=ramp*(.5*.4-.25*(1-elapsed/.08));
                    mixed&=std::abs(block.samples[frame*2]-expected)<.002;++fadeFrames;
                }
                if(elapsed>.11 && elapsed<.15){
                    survived&=std::abs(block.samples[frame*2]-ramp*.5*.4)<.0004;++remainingFrames;
                }
            }
            check(mixed && fadeFrames>2000,"two music voices mix independent trigger gains and a sample-ramped fade");
            check(survived && remainingFrames>1000,"stopping channel 7 leaves channel 0 playing");
        }
    }
    receiver.reset();
}

static void malformedV5Test() {
    receiver=std::make_unique<Receiver>();
    {
        SyncedSender sender(3600000000000LL);
        check(sender.synchronize(),"validation test synchronizes its sender");
        SongLinkPacket p;p.flags=1;p.epoch=718;sender.send(p,os_gettime_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        p.epoch=999;p.fadeCount=9;sender.send(p,os_gettime_ns());
        p.fadeCount=0;p.triggerGain=std::numeric_limits<float>::quiet_NaN();sender.send(p,os_gettime_ns());
        p.triggerGain=1;p.channelID=SONG_LINK_MAX_CHANNELS;sender.send(p,os_gettime_ns());
        p.channelID=0;p.flags=7;p.loopStart=2;p.loopEnd=1;sender.send(p,os_gettime_ns());
        p.loopStart=std::numeric_limits<double>::quiet_NaN();p.loopEnd=2;sender.send(p,os_gettime_ns());
        p.loopStart=1;p.loopEnd=1;sender.send(p,os_gettime_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        check(receiver->get().packet.epoch==718,"v5 rejects oversized fades, nonfinite gain, invalid channels and invalid loop bounds");
        // FMOD can hold two fade points at the same DSP clock
        p=SongLinkPacket{};p.flags=1;p.epoch=720;p.fadeCount=2;p.fades[0]={1000000,.5f};p.fades[1]={1000000,0};
        sender.send(p,os_gettime_ns());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        check(receiver->get().packet.epoch==720,"v5 accepts fade points that share a time");
    }
    receiver.reset();
}

static void shortLoopTest() {
    receiver=std::make_unique<Receiver>();resetOutput();
    {
        SyncedSender sender(3600000000000LL);
        check(sender.synchronize(),"short-loop test synchronizes its sender");
        SongLinkPacket loop;loop.flags=7;loop.epoch=718;loop.position=1.004;loop.rate=2;
        loop.loopStart=1;loop.loopEnd=1.01;loop.offset=.3;loop.musicVolume=.5f;loop.triggerGain=.8f;
        std::strcpy(loop.path,"artifacts/audio-tests/ramp.wav");
        auto capture=os_gettime_ns();std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sender.send(loop,capture);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto mapped=receiver->get().packet.timestamp;
        check(receiver->get().packet.epoch==718,"delayed 10 ms loop packet is accepted after clock conversion");
        SongLinkPacket other;other.flags=3;other.epoch=719;other.channelID=7;
        other.position=2;other.rate=.5;other.offset=-.1;other.musicVolume=.2f;
        std::strcpy(other.path,"artifacts/audio-tests/negative.wav");
        sender.send(other,capture);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            std::this_thread::sleep_for(std::chrono::milliseconds(160));
            unsigned count=0;bool correct=true;
            // Voices fade in over 5 ms when the source starts, which scheduling can delay into this window.
            auto faded=output().empty()?0:output().front().timestamp+5000000;
            for(const auto& block:output())for(unsigned frame=0;frame<480;++frame){
                auto at=block.timestamp+frame*1000000000ULL/48000;
                double elapsed=double(int64_t(at)-int64_t(mapped))/1e9;
                if(elapsed<.12 || elapsed>.20 || at<faded)continue;
                double loopPosition=loop.loopStart+std::fmod(loop.position+elapsed*loop.rate-loop.loopStart,loop.loopEnd-loop.loopStart);
                double expected=(.1+(loopPosition+loop.offset)*.1)*loop.musicVolume*loop.triggerGain
                    -(.1+(other.position+elapsed*other.rate+other.offset)*.1)*other.musicVolume;
                double phase=(loopPosition-loop.loopStart)/loop.rate;
                if(phase<2./48000 || phase>(loop.loopEnd-loop.loopStart)/loop.rate-2./48000)continue;
                if(std::abs(block.samples[frame*2]-expected)>.00008){
                    if(correct)std::cerr<<"Loop mismatch: elapsed="<<elapsed<<", expected="<<expected
                        <<", actual="<<block.samples[frame*2]<<'\n';
                    correct=false;
                }
                ++count;
            }
            check(correct && count>3000,"delayed 10 ms source loop wraps before user offset at 2x while another voice continues");
        }
    }
    receiver.reset();
}

static void foreignEffectsTest() {
    receiver=std::make_unique<Receiver>();resetOutput();
    {
        SyncedSender sender(3600000000000LL);
        check(sender.synchronize(),"effects test synchronizes its sender");
        auto base=os_gettime_ns();
        SongLinkPacket silentVoice;silentVoice.flags=3;silentVoice.position=1;silentVoice.musicVolume=0;
        std::strcpy(silentVoice.path,"artifacts/audio-tests/ramp.wav");
        sender.send(silentVoice,base-10000000);
        silentVoice.channelID=7;sender.send(silentVoice,base-10000000);
        SongLinkPacket global;global.channelID=-1;global.flags=1;global.effectsVolume=.2f;
        sender.send(global,base-10000000);
        global.flags=0;sender.send(global,base+30000000);
        global.flags=1;global.effectsVolume=.8f;sender.send(global,base+60000000);
        auto effectsSocket=socket(AF_INET,SOCK_DGRAM,0);
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        for(int i=-3;i<11;++i){
            EffectsPacketV2 p;p.sessionID=sender.session;p.frames=480;p.sampleRate=48000;
            p.timestamp=sender.stamp(uint64_t(int64_t(base)+i*10000000LL));
            std::fill_n(p.samples,960,.25f);
            sendto(effectsSocket,reinterpret_cast<const char*>(&p),44+p.frames*8,0,
                reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
        }
        closeSocket(effectsSocket);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto mappedBase=receiver->get().packet.timestamp-60000000;
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            std::this_thread::sleep_for(std::chrono::milliseconds(190));
            bool correct=true;unsigned frames=0;
            for(const auto& b:output())for(unsigned i=0;i<480;++i){
                auto at=b.timestamp+i*1000000000ULL/48000;
                auto delta=int64_t(at)-int64_t(mappedBase);
                if(delta<0 || delta>100000000)continue;
                auto expected=delta<30000000?.05:delta<60000000?0:.2;
                correct&=std::abs(b.samples[i*2]-expected)<.00001;++frames;
            }
            check(correct && frames>4000,"v2 effects follow exact negotiated timeline boundaries, mixed once with two active voices");
        }
    }
    receiver.reset();
}

static void changedClockTest() {
    receiver=std::make_unique<Receiver>();resetOutput();
    {
        SyncedSender sender(3600000000000LL);
        check(sender.synchronize(),"clock-change test synchronizes its sender");
        SongLinkPacket old;old.flags=3;old.position=1;
        std::strcpy(old.path,"artifacts/audio-tests/negative.wav");
        auto oldCapture=os_gettime_ns();sender.send(old,oldCapture);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        sender.clockOffset+=7200000000000LL;
        check(sender.synchronize(819),"same-session clock-origin change recalibrates after simulated suspend");
        auto prior=receiver->playbackWindow(oldCapture,oldCapture+1);
        check(prior.size()==1 && prior.front().flags==0,"recalibration clears buffered audio state from the old clock mapping");
        SongLinkPacket current;current.flags=3;current.position=2;current.epoch=820;
        std::strcpy(current.path,"artifacts/audio-tests/ramp.wav");
        auto capture=os_gettime_ns();sender.send(current,capture);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        {
            SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            unsigned count=0;bool correct=true;
            for(const auto& b:output())if(b.timestamp>capture+20000000 && b.timestamp<capture+120000000){
                auto expected=.1+(2+double(b.timestamp-capture)/1e9)*.1;
                correct&=std::abs(b.samples[0]-expected)<.0004;++count;
            }
            check(correct && count>=6,"playback resumes at the sampled position after a clock-origin change");
        }
    }
    receiver.reset();
}
// Ten minutes of 1024-frame blocks from an output device running 200 ppm slow, mixed with
// +-5 ms of thread scheduling jitter. Effects timestamps must stay back to back within one
// sample (a reset would leave a gap OBS hears) and within 3 ms of the real wall clock.
static void tapClockDriftTest() {
    separate_song::TapClock clock;
    uint64_t wall=1000000000,previousEnd=0;
    int64_t worstSeam=0,worstLag=0;
    for(int block=0;block<28125;++block){
        auto observed=uint64_t(int64_t(wall)+(block*7919%11-5)*1000000LL);
        auto stamp=clock.stamp(observed,48000);
        if(block)worstSeam=std::max(worstSeam,std::abs(int64_t(stamp-previousEnd)));
        if(block>2000)worstLag=std::max(worstLag,std::abs(int64_t(wall-stamp)));
        clock.advance(1024);previousEnd=stamp+1024*1000000000ULL/48000;
        wall+=uint64_t(1024*1000000000.0/48000*1.0002);
    }
    check(worstSeam<=20834 && worstLag<3000000,"effects timestamps follow a drifting output clock without jumping");
}
// Back-to-back packets of a bright 3 kHz SFX, not aligned to OBS's sample grid. Each packet's
// last sample must blend into the next packet's first; holding it instead crackles at ~94 Hz.
static void effectsSeamTest(unsigned sampleRate) {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    sendState(sender,SongLinkPacket{},os_gettime_ns()-200000000); // legacy SFX need a song sender first
    EffectsPacket initial;initial.frames=1;initial.sampleRate=48000;initial.timestamp=os_gettime_ns();
    sendto(sender,reinterpret_cast<const char*>(&initial),36+initial.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto base=os_gettime_ns(),first=base+7777;
    // Linux dates legacy effects by receipt. A ramp ending well before the seams reveals that
    // shift exactly: its value at any rendered time is the time since the ramp started.
    EffectsPacket marker;marker.sequence=50;marker.frames=512;marker.sampleRate=8000;marker.timestamp=base-100000000;
    for(unsigned i=0;i<512;++i)marker.samples[i*2]=marker.samples[i*2+1]=i/1024.f;
    sendto(sender,reinterpret_cast<const char*>(&marker),36+marker.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    SongLinkPacket p;p.flags=1;sendState(sender,p,base-50000000);
    auto tone=[&](double frame){return .25*std::sin(frame*2*3.14159265358979*3000/sampleRate);};
    unsigned frames=sampleRate/100;
    for(unsigned k=0;k<12;++k){
        EffectsPacket e;e.sequence=100+k;e.frames=frames;e.sampleRate=sampleRate;e.timestamp=first+k*10000000ULL;
        for(unsigned i=0;i<frames;++i)e.samples[i*2]=e.samples[i*2+1]=float(tone(k*double(frames)+i));
        sendto(sender,reinterpret_cast<const char*>(&e),36+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::array<float,2> ramp{};
    auto probe=marker.timestamp+32000000;receiver->mixEffects(ramp.data(),1,probe,1);
    auto mapped=first+(int64_t(probe)-int64_t(std::llround(ramp[0]*1024e9/8000))-int64_t(marker.timestamp));
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        double worst=0;unsigned count=0;
        for(const auto& b:output())for(size_t i=0;i<480;++i){
            auto at=b.timestamp+i*1000000000/48000;
            if(at<mapped+10000000 || at>mapped+100000000)continue;
            double u=(double(at)-double(mapped))*sampleRate/1e9,whole=std::floor(u);
            double expected=tone(whole)+(tone(whole+1)-tone(whole))*(u-whole);
            worst=std::max(worst,std::abs(b.samples[i*2]-expected));++count;
        }
        check(ramp[0]>0 && count>4000 && worst<.0001,sampleRate==44100?
            "44.1 kHz effects blend across packet seams":"48 kHz effects blend across packet seams");
    }
    receiver.reset();closeSocket(sender);
}
static void effectsGapTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    sendState(sender,SongLinkPacket{},os_gettime_ns()-200000000); // legacy SFX need a song sender first
    auto base=os_gettime_ns();
    // Consecutive sequence numbers do not prove continuity: the producer may have stalled.
    for(unsigned k=0;k<2;++k){
        EffectsPacket e;e.sequence=100+k;e.frames=441;e.sampleRate=44100;e.timestamp=base+k*100000000ULL;
        std::fill_n(e.samples,e.frames*2,k?0.f:.5f);
        sendto(sender,reinterpret_cast<const char*>(&e),36+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Legacy effects are dated by receipt on Linux. Locate the actual first sample on the
    // rendered timeline instead of assuming the sender's timestamp was left unchanged.
    std::array<float,9600> observed{};
    receiver->mixEffects(observed.data(),4800,base,1);
    auto first=std::find(observed.begin(),observed.end(),.5f);
    bool received=first!=observed.end();
    auto mappedBase=base+uint64_t((first-observed.begin())/2)*1000000000/48000;
    std::array<float,960> audio{};
    receiver->mixEffects(audio.data(),480,mappedBase+50000000,1);
    check(received && std::all_of(audio.begin(),audio.end(),[](float s){return s==0;}),
        "SFX interpolation leaves real gaps silent at 44.1 kHz");
    receiver.reset();closeSocket(sender);
}
// GD's position runs 20 ms ahead of what OBS is playing (FMOD's output clock drifted). The
// voice must catch up by playing slightly fast, not stay off or jump with a seek.
static void driftCorrectionTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    auto base=os_gettime_ns();
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    sendState(sender,p,base);
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        for(int i=0;i<13;++i){                                // keep GD's reports fresh, 20 ms ahead
            auto at=base+50000000ULL+i*50000000ULL;
            while(os_gettime_ns()+60000000<at)std::this_thread::sleep_for(std::chrono::milliseconds(1));
            p.position=1.07+i*.05;sendState(sender,p,at);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        // ramp.wav plays .1+seconds*.1, so a sample gives back the song position OBS played
        auto behind=[&](uint64_t time){
            for(const auto& b:output())if(time>=b.timestamp && time<b.timestamp+10000000){
                size_t frame=size_t((time-b.timestamp)*48000/1000000000);
                double played=(b.samples[frame*2]-.1)/.1,reported=1.07+double(int64_t(time)-int64_t(base)-50000000)/1e9;
                return reported-played;
            }
            return 0.;
        };
        double early=behind(base+100000000),late=behind(base+600000000);
        if(early<.015 || late>early-.0015)std::cerr<<"Drift correction: behind "<<early*1000<<" ms, then "<<late*1000<<" ms\n";
        check(early>.015 && late<early-.0015,"a 20 ms position error is steered out without a seek");
    }
    receiver.reset();closeSocket(sender);
}
static void localFilesOnlyTest() {
    Decoder decoder;bool refused=true;
    for(const char* path:{"https://example.com/song.mp3","\\\\server\\share\\song.mp3","//server/share/song.mp3"})
        refused&=!decoder.open(path) && decoder.error=="Only local song files can be played.";
    check(refused && decoder.open("artifacts/audio-tests/ramp.wav"),"decoder plays local files and refuses URLs and network shares");
}
static void senderTest() {
    separate_song::Snapshot s;s.timestamp=monotonicNowNs();s.position=1.25;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    separate_song::Bridge b;check(b.start(),"sender opens its socket");b.publish(s);
    check(published.version==5 && published.timestamp==s.timestamp && published.position==s.position,
        "sender preserves sampling time through publication delay");
    check(std::abs(double(int64_t(monotonicNowNs())-int64_t(os_gettime_ns())))<1000000,"native sender and OBS test clock use compatible sampling units");
}
#include "receiver-tests.inc"
#include "deadline-tests.inc"
#include "recovery-tests.inc"

int main(){
#ifdef __APPLE__
    int relativePriority=0;
    qos_class_t qos=QOS_CLASS_UNSPECIFIED;
    check(pthread_get_qos_class_np(pthread_self(),&qos,&relativePriority)==0,
        "macOS test sender reports initial scheduling");
    std::cout<<"macOS test thread initial QoS: "<<unsigned(qos)<<", relative priority: "<<relativePriority<<'\n';
    check(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0)==0,
        "macOS test sender uses foreground audio scheduling");
    check(pthread_get_qos_class_np(pthread_self(),&qos,&relativePriority)==0,
        "macOS test sender reports effective scheduling");
    auto timingStart=os_gettime_ns();
    for(unsigned i=0;i<20;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::cout<<"macOS test thread effective QoS: "<<unsigned(qos)<<", relative priority: "<<relativePriority
        <<", twenty 10 ms sleeps: "<<double(os_gettime_ns()-timingStart)/1e6<<" ms\n";
#endif
#ifdef _WIN32
    _putenv_s("OBS_JUKEBOX_TEST_PORT","49177");
#else
    setenv("OBS_JUKEBOX_TEST_PORT","49177",1);
#endif
    writeRamp("artifacts/audio-tests/ramp.wav");
    writeRamp("artifacts/audio-tests/negative.wav",true);
    if(std::getenv("OBS_JUKEBOX_RECOVERY_ONLY")){sourceRecoveryTest(false);sourceRecoveryTest(true);sourceResyncRecoveryTest();decoderSeekRecoveryTest();return failures?1:0;}
    pauseTest();equalTimestampStopTest();delayedPositionTest();transitionTest();declickTest();staleVoiceTest();effectsTransitionTest();staleStatusEffectsTest();clockNoiseTest();clockSlewTest();protocolTest();
    mainThreadStallTest(Silence::MainThreadStall);mainThreadStallTest(Silence::Crash);mainThreadStallTest(Silence::VoiceOnly);
    v5ClockCorrectedPauseTest();foreignClockTest(3600000000000LL);
    foreignClockTest(-int64_t(std::min<uint64_t>(os_gettime_ns()/2,3600000000000ULL)));
    multipleVoiceFadeTest();malformedV5Test();shortLoopTest();foreignEffectsTest();changedClockTest();localFilesOnlyTest();
    tapClockDriftTest();effectsSeamTest(44100);effectsSeamTest(48000);effectsGapTest();driftCorrectionTest();senderTest();
    receiverIsolationTest();receiverEffectsBoundaryTest();receiverFloodTest();receiverLifetimeTest();
    sourceRecoveryTest(false);sourceRecoveryTest(true);sourceResyncRecoveryTest();decoderSeekRecoveryTest();
    musicDeadlineTest(false);musicDeadlineTest(true);
    return failures?1:0;
}
