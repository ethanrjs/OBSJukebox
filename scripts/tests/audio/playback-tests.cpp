#include "../../../obs-plugin/separate-song.cpp"
#include <iostream>
#include <fstream>
#include "../../../src/MonotonicClock.hpp"
#include "../../../src/Bridge.hpp"

static SongLinkPacket published;
static int captureSend(SongSocket,const char* data,int size,int,const sockaddr*,int) {
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
    std::array<char,1304> wire{};
    p.version=version;
    std::memcpy(wire.data(),&p,1296);
    std::memcpy(wire.data()+1296,&capture,8);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    auto bytes=version==2?1288:version==3?1296:1304;
    auto sent=sendto(socket,wire.data(),bytes,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
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
        // Legacy senders have no capture stamp. Arrival must not cut already buffered music.
        check(firstSilent>=pauseAt-2000000 && firstSilent<=pauseAt+15000000,"pause takes effect on the audio timeline");
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
    auto base=os_gettime_ns();
    struct Event {uint64_t at;SongLinkPacket p;};
    std::vector<Event> events;
    SongLinkPacket p;p.flags=3;p.position=1;std::strcpy(p.path,"artifacts/audio-tests/ramp.wav");
    auto add=[&](int milliseconds){auto at=uint64_t(int64_t(base)+int64_t(milliseconds)*1000000);events.push_back({at,p});sendState(sender,p,at);};
    add(-100);
    p.flags=1;add(35); // Pause.
    p.flags=3;p.position=1.4;add(40); // Resume.
    p.flags=1;add(50); // Death.
    p.flags=3;p.epoch=2;p.position=2;p.offset=.1;p.rate=2;p.musicVolume=.5;add(60); // Restart.
    p.epoch=3;p.position=2.035;add(65); // Small seek that requires the epoch.
    p.position=2.045;p.musicVolume=.25;add(70); // Volume only.
    p.position=2.055;
    std::strcpy(p.path,"artifacts/audio-tests/negative.wav");add(75); // Song selection.
    p.flags=0;add(85); // Disable.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        std::this_thread::sleep_for(std::chrono::milliseconds(210));
        bool correct=true;std::vector<unsigned> counts(events.size());
        for(const auto& block:output())for(size_t frame=0;frame<480;++frame){
            auto at=block.timestamp+frame*1000000000/48000;
            size_t event=0;while(event+1<events.size() && events[event+1].at<=at)++event;
            const auto& current=events[event];
            double expected=0;
            if((current.p.flags&3)==3){
                auto position=current.p.position+current.p.offset+double(int64_t(at)-int64_t(current.at))/1e9*current.p.rate;
                expected=(.1+position*.1)*current.p.musicVolume*(std::strstr(current.p.path,"negative")?-1:1);
            }
            if(std::abs(block.samples[frame*2]-expected)>=.0002){
                if(correct)std::cerr<<"First mismatch: event "<<event<<", time "<<double(int64_t(at)-int64_t(base))/1e6<<" ms, expected "<<expected<<", actual "<<block.samples[frame*2]<<'\n';
                correct=false;
            }
            ++counts[event];
        }
        check(correct && std::all_of(counts.begin(),counts.end(),[](unsigned n){return n>0;}),
            "pause, resume, death, restart, path and gain changes occur at the correct sample");
    }
    receiver.reset();closeSocket(sender);
}
static void effectsTransitionTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);resetOutput();
    auto base=os_gettime_ns();
    SongLinkPacket p;p.flags=1;p.effectsVolume=.2;sendState(sender,p,base-100000000);
    p.flags=0;sendState(sender,p,base+30000000);
    p.flags=1;p.effectsVolume=.8;sendState(sender,p,base+60000000);
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
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
        check(correct && count>4000,"effects enable and gain changes follow the music timeline");
    }
    receiver.reset();closeSocket(sender);
}
static void protocolTest() {
    receiver=std::make_unique<Receiver>();auto sender=socket(AF_INET,SOCK_DGRAM,0);
    SongLinkPacket p;p.flags=3;p.position=1;
    sendState(sender,p,0,2);std::this_thread::sleep_for(std::chrono::milliseconds(15));
    auto v2=receiver->get();
    check(v2.connected && v2.packet.version==2 && v2.packet.musicVolume==1 && v2.packet.effectsVolume==1,"v2 packets retain default gains");
    auto at=os_gettime_ns();sendState(sender,p,at);std::this_thread::sleep_for(std::chrono::milliseconds(15));
    p.epoch=99;sendState(sender,p,at-1000000);sendState(sender,p,at+2000000000);
    sendState(sender,p,0);sendState(sender,p,os_gettime_ns(),5);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    check(receiver->get().packet.timestamp==at && receiver->get().packet.epoch==0,"reordered, stale, future and unsupported packets are ignored");
    auto before=receiver->playbackWindow(at-1,at+1);
    check(before.size()==2 && before.front().version==2 && before.back().timestamp==at,"history retains the state before a block transition");
    auto initial=receiver->playbackWindow(1,2);
    check(initial.size()==1 && initial.front().flags==0,"future state cannot start music before its timestamp");
    receiver.reset();closeSocket(sender);
}
static void senderTest() {
    separate_song::Snapshot s;s.timestamp=separate_song::monotonicNs();s.position=1.25;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    separate_song::Bridge b;check(b.start(),"sender opens its socket");b.publish(s);
    check(published.version==4 && published.timestamp==s.timestamp && published.position==s.position,
        "sender preserves sampling time through publication delay");
    check(std::abs(double(int64_t(separate_song::monotonicNs())-int64_t(os_gettime_ns())))<1000000,"sender and OBS use the same monotonic clock");
}
int main(){
    _putenv_s("OBS_JUKEBOX_TEST_PORT","49177");
    writeRamp("artifacts/audio-tests/ramp.wav");
    writeRamp("artifacts/audio-tests/negative.wav",true);
    pauseTest();delayedPositionTest();transitionTest();effectsTransitionTest();protocolTest();senderTest();
    return failures?1:0;
}
