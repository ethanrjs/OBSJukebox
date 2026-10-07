#include "../src/Socket.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include "../src/LinkPacket.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <memory>
#include "Decoder.hpp"
#include <deque>
#include <cstdio>
#include <cstdlib>

OBS_DECLARE_MODULE()
MODULE_EXPORT const char* obs_module_description(void) { return "OBS Jukebox: Geometry Dash custom song and sound effects"; }
MODULE_EXPORT const char* obs_module_name(void) { return "OBS Jukebox"; }
MODULE_EXPORT const char* obs_module_author(void) { return "babbur"; }

using Clock = std::chrono::steady_clock;
struct LinkState { SongLinkPacket packet; Clock::time_point received{}; bool connected=false; };
class Receiver {
    SongSocket sock=BAD_SOCKET;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    LinkState state;
    std::deque<EffectsPacket> effects;
    int64_t clockOffset=0;bool clockSet=false;
public:
    Receiver() {
        if(!socketsReady())return;
        sock=socket(AF_INET,SOCK_DGRAM,0);
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(sock==BAD_SOCKET || bind(sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0){
            if(sock!=BAD_SOCKET)closeSocket(sock);sock=BAD_SOCKET;
            blog(LOG_ERROR,"[OBS Jukebox] Could not bind local game link on port %d",SONG_LINK_PORT);return;
        }
        nonblocking(sock);
        int receiveBuffer=1024*1024;
        setsockopt(sock,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&receiveBuffer),sizeof(receiveBuffer));
        worker=std::thread([this]{
            bool sawPacket=false;
            FILE* reference=nullptr;
            if(auto path=std::getenv("SEPARATE_SONG_REFERENCE_FILE"))reference=std::fopen(path,"wb");
            while(!stop){
                std::array<unsigned char,sizeof(EffectsPacket)> buffer{};
                auto size=recv(sock,reinterpret_cast<char*>(buffer.data()),buffer.size(),0);
                if(size>=36 && !memcmp(buffer.data(),"GDSFX1\0",8)){
                    EffectsPacket effect;std::memcpy(&effect,buffer.data(),size);
                    uint64_t now=os_gettime_ns();
                    #if defined(__linux__)
                    if(!clockSet){clockOffset=int64_t(now)-int64_t(effect.timestamp);clockSet=true;}
#endif
                    effect.timestamp=uint64_t(int64_t(effect.timestamp)+clockOffset);
                    if(effect.version!=1 || effect.frames<1 || effect.frames>512 || size!=36+effect.frames*8 ||
                       effect.sampleRate<8000 || effect.sampleRate>192000 || effect.timestamp>now+1000000000 ||
                       effect.timestamp+1000000000<now || effect.stream>1)continue;
                    bool finite=true;for(unsigned i=0;i<effect.frames*2;++i)finite&=std::isfinite(effect.samples[i]);
                    if(!finite)continue;
                    if(effect.stream==1){if(reference){std::fwrite(&effect,sizeof(effect),1,reference);std::fflush(reference);}continue;}
                    std::lock_guard lock(mutex);effects.push_back(effect);
                    while(effects.size()>64)effects.pop_front();
                    continue;
                }
                SongLinkPacket p{};
                if(size==sizeof(p) || size==SONG_LINK_V2_SIZE)std::memcpy(&p,buffer.data(),size);
                if(size>0 && !sawPacket){sawPacket=true;blog(LOG_INFO,"[OBS Jukebox] Link packet: %zd bytes, version %u",size,p.version);}
                if(((size==sizeof(p) && p.version==3) || (size==SONG_LINK_V2_SIZE && p.version==2)) &&
                   !memcmp(p.magic,"GDSONG1",8) &&
                   std::isfinite(p.musicVolume) && p.musicVolume>=0 && p.musicVolume<=1 &&
                   std::isfinite(p.effectsVolume) && p.effectsVolume>=0 && p.effectsVolume<=1 &&
                   std::isfinite(p.position) && p.position>=-600 && p.position<86400 &&
                   std::isfinite(p.offset) &&
                   std::isfinite(p.rate) && p.rate>=.25 && p.rate<=4){
                    p.status[sizeof(p.status)-1]=0;p.level[sizeof(p.level)-1]=0;
                    p.song[sizeof(p.song)-1]=0;p.path[sizeof(p.path)-1]=0;

                    std::lock_guard lock(mutex);state={p,Clock::now(),true};
                }else {
                    fd_set readable;FD_ZERO(&readable);FD_SET(sock,&readable);
                    timeval timeout{};timeout.tv_usec=10000;
                    select(int(sock)+1,&readable,nullptr,nullptr,&timeout);
                }
            }
            if(reference)std::fclose(reference);
        });
    }
    ~Receiver(){stop=true;if(worker.joinable())worker.join();if(sock!=BAD_SOCKET)closeSocket(sock);}
    LinkState get(){std::lock_guard lock(mutex);return state;}
    void mixEffects(float* output,size_t frames,uint64_t start,float gain){
        std::lock_guard lock(mutex);
        for(const auto& p:effects){
            double delta=(double(start)-double(p.timestamp))/1e9;
            if(delta>double(p.frames)/p.sampleRate || delta+double(frames)/48000<0)continue;
            for(size_t i=0;i<frames;++i){
                double at=(delta+double(i)/48000)*p.sampleRate;
                if(at<0 || at>=p.frames)continue;
                unsigned a=unsigned(at),b=std::min(a+1,p.frames-1);float blend=at-a;
                for(unsigned ch=0;ch<2;++ch)output[i*2+ch]+=gain*(p.samples[a*2+ch]+(p.samples[b*2+ch]-p.samples[a*2+ch])*blend);
            }
        }
    }
};
static std::unique_ptr<Receiver> receiver;

struct SongSource {
    obs_source_t* source;
    std::atomic<bool> stop{false},active{false};
    std::mutex mutex;
    std::string detail;
    std::thread worker;
    explicit SongSource(obs_source_t* s):source(s),
        detail("Select the OBS checkbox beside a Jukebox song."){}
    void run(){
        Decoder decoder;
        std::string loadedPath;unsigned lastEpoch=~0u;bool wasPlaying=false;
        auto retryAt=Clock::time_point{};
        std::array<float,960> audio{};
        auto next=Clock::now();
        constexpr uint64_t bufferingNs=40000000;
        uint64_t timestamp=os_gettime_ns()-bufferingNs;
        while(!stop){
            next+=std::chrono::milliseconds(10);
            auto link=receiver->get();auto p=link.packet;
            double age=std::chrono::duration<double>(Clock::now()-link.received).count();
            bool connected=link.connected && age<.4;
            std::string newPath=connected?p.path:"";
            bool pathChanged=newPath!=loadedPath;
            if(pathChanged || (!newPath.empty() && !decoder.ready() && Clock::now()>=retryAt)){
                decoder.open(newPath);loadedPath=newPath;lastEpoch=~0u;
                retryAt=Clock::now()+std::chrono::seconds(1);
                if(pathChanged || decoder.ready())blog(LOG_INFO,"[OBS Jukebox] Decoder %s: %s",decoder.ready()?"ready":"unavailable",p.song);
            }
            double audioAge=age+(double(timestamp)-double(os_gettime_ns()))/1e9;
            double target=p.position+p.offset+(connected&&(p.flags&2)?audioAge*p.rate:0);
            bool playing=connected && (p.flags&3)==3 && active && std::isfinite(target) &&
                target>=0 && target<=31536000.0 && decoder.ready();
            audio.fill(0);
            if(playing){
                if(lastEpoch!=p.epoch || !wasPlaying || std::abs(decoder.position-target)>.04)
                    playing=decoder.seek(target);
                if(playing)decoder.render(audio.data(),480,p.rate);
            }
            for(auto& sample:audio)sample*=p.musicVolume;
            if(active && connected && (p.flags&1))receiver->mixEffects(audio.data(),480,timestamp,p.effectsVolume);
            if(playing!=wasPlaying)blog(LOG_INFO,"[OBS Jukebox] %s (%s)",playing?"Playing":"Stopped",connected?p.status:"game disconnected");
            wasPlaying=playing;lastEpoch=p.epoch;
            {
                std::lock_guard lock(mutex);
                detail=!connected?"Waiting for Geometry Dash":
                    !p.song[0]?"Select the OBS checkbox beside a Jukebox song.":!decoder.error.empty()?decoder.error:
                    std::string(p.song)+" | "+p.status+" | Attempt "+std::to_string(p.attempt);
            }
            obs_source_audio block{};
            block.data[0]=reinterpret_cast<const uint8_t*>(audio.data());block.frames=480;
            block.speakers=SPEAKERS_STEREO;block.format=AUDIO_FORMAT_FLOAT;block.samples_per_sec=48000;
            block.timestamp=timestamp;obs_source_output_audio(source,&block);
            timestamp+=10000000;
            auto now=Clock::now();if(next<now-std::chrono::milliseconds(100)){
                next=now;timestamp=os_gettime_ns()-bufferingNs;
            }
            std::this_thread::sleep_until(next);
        }
    }
    ~SongSource(){stop=true;if(worker.joinable())worker.join();}
};

static const char* sourceName(void*){return "GD Sounds";}
static void* create(obs_data_t* settings,obs_source_t* source){
    auto s=new SongSource(source);s->worker=std::thread([s]{s->run();});return s;
}
static obs_properties_t* properties(void* data){
    auto props=obs_properties_create();
    obs_properties_add_text(props,"selection","In Jukebox, Game selects the in-game song. OBS selects the OBS song. Click it again to clear the OBS song.",OBS_TEXT_INFO);
    std::string detail="Playback follows GD automatically.";
    if(data){auto s=static_cast<SongSource*>(data);std::lock_guard lock(s->mutex);detail=s->detail;}
    obs_properties_add_text(props,"link",detail.c_str(),OBS_TEXT_INFO);
    obs_properties_add_text(props,"help","Includes the selected custom song and GD sound effects, including clicks routed through GD's effects engine. Keep Audio Monitoring at Monitor Off and disable audio on other GD/desktop captures to prevent duplicate music.",OBS_TEXT_INFO);
    return props;
}
bool obs_module_load(void){
    receiver=std::make_unique<Receiver>();
    obs_source_info info{};info.id="gd_alternate_song";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_AUDIO;
    info.get_name=sourceName;info.create=create;info.destroy=[](void* d){delete static_cast<SongSource*>(d);};
    info.get_properties=properties;
    info.icon_type=OBS_ICON_TYPE_AUDIO_OUTPUT;
    info.activate=[](void* d){static_cast<SongSource*>(d)->active=true;};
    info.deactivate=[](void* d){static_cast<SongSource*>(d)->active=false;};
    obs_register_source(&info);
    blog(LOG_INFO,"[OBS Jukebox] Native GD Sounds source registered");return true;
}
void obs_module_unload(void){receiver.reset();}
