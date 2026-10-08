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
#include "LinkClock.hpp"
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#if defined(__linux__)
#include "LinuxPaths.hpp"
#include "LinuxAudioClock.hpp"
#endif

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
    std::array<std::deque<SongLinkPacket>,SONG_LINK_MAX_CHANNELS+1> playback;
    std::deque<EffectsPacket> effects,pendingEffects;
    LinkClock clock;
    uint64_t session=0,lastProbe=0;
    std::deque<uint64_t> probes;
    sockaddr_in peer{};
    std::deque<SongLinkPacket> pending;
#if defined(__linux__)
    LinuxAudioClock effectsClock;
#endif
    void clearTimeline(){for(auto& voice:playback)voice.clear();effects.clear();state.connected=false;}
    void accept(SongLinkPacket p,uint64_t now){
        if(p.version>=5 && !clock.translate(p.timestamp))return;
        if(p.timestamp>now+100000000 || p.timestamp+1000000000<now)return;
        auto& voice=playback[p.channelID+1];
        auto next=std::lower_bound(voice.begin(),voice.end(),p.timestamp,[](const auto& a,uint64_t t){return a.timestamp<t;});
        if(next!=voice.end() && next->timestamp==p.timestamp)return;
        voice.insert(next,p);while(voice.size()>256)voice.pop_front();
        if(!state.connected || p.timestamp>=state.packet.timestamp)state={p,Clock::now(),true};
    }
    void probe(uint64_t now){
        if(!session || now-lastProbe<250000000)return;
        ClockSyncPacket p;p.sessionID=session;p.t1=now;
        probes.push_back(now);while(probes.size()>8)probes.pop_front();lastProbe=now;
        sendto(sock,reinterpret_cast<const char*>(&p),sizeof(p),0,reinterpret_cast<sockaddr*>(&peer),sizeof(peer));
    }
public:
    Receiver(){
        if(!socketsReady())return;
        sock=socket(AF_INET,SOCK_DGRAM,0);
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(sock==BAD_SOCKET || bind(sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0){
            if(sock!=BAD_SOCKET)closeSocket(sock);sock=BAD_SOCKET;
            blog(LOG_ERROR,"[OBS Jukebox] Could not bind local game link on port %d",receiverPort());return;
        }
        nonblocking(sock);int receiveBuffer=1024*1024;
        setsockopt(sock,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&receiveBuffer),sizeof(receiveBuffer));
        worker=std::thread([this]{
            FILE* reference=nullptr;
            if(auto path=std::getenv("SEPARATE_SONG_REFERENCE_FILE"))reference=std::fopen(path,"wb");
            while(!stop){
                std::array<unsigned char,sizeof(EffectsPacketV2)+1> buffer{};
                sockaddr_in from{};
#ifdef _WIN32
                int length=sizeof(from);
#else
                socklen_t length=sizeof(from);
#endif
                auto size=recvfrom(sock,reinterpret_cast<char*>(buffer.data()),buffer.size(),0,reinterpret_cast<sockaddr*>(&from),&length);
                auto now=os_gettime_ns();probe(now);
                if(size<=0){fd_set readable;FD_ZERO(&readable);FD_SET(sock,&readable);timeval timeout{};timeout.tv_usec=2000;select(int(sock)+1,&readable,nullptr,nullptr,&timeout);continue;}
                if(from.sin_addr.s_addr!=htonl(INADDR_LOOPBACK))continue;
                if(size==sizeof(ClockSyncPacket) && !memcmp(buffer.data(),"GDCLK1\0",8)){
                    ClockSyncPacket p;memcpy(&p,buffer.data(),sizeof(p));
                    auto found=std::find(probes.begin(),probes.end(),p.t1);
                    if(p.version!=1 || p.kind!=2 || p.sessionID!=session || found==probes.end() || from.sin_port!=peer.sin_port)continue;
                    probes.erase(found);bool changed=false;
                    std::lock_guard lock(mutex);
                    if(clock.observe(p,now,changed)){
                        if(changed)clearTimeline();
                        for(auto& waiting:pending)accept(waiting,now);pending.clear();
                        for(auto waiting:pendingEffects)if(clock.translate(waiting.timestamp)&&waiting.timestamp<=now+1000000000&&waiting.timestamp+1000000000>=now)effects.push_back(waiting);
                        pendingEffects.clear();while(effects.size()>128)effects.pop_front();
                    }
                    continue;
                }
                bool modernEffects=size>=44 && !memcmp(buffer.data(),"GDSFX2\0",8);
                if(modernEffects || (size>=36 && !memcmp(buffer.data(),"GDSFX1\0",8))){
                    EffectsPacket effect;uint64_t effectSession=0;
                    if(modernEffects){
                        if(size>sizeof(EffectsPacketV2))continue;
                        EffectsPacketV2 p;memcpy(&p,buffer.data(),size);
                        if(p.version!=2 || p.frames>512 || size!=44+p.frames*8)continue;
                        effect.sequence=p.sequence;effect.sampleRate=p.sampleRate;effect.frames=p.frames;effect.timestamp=p.timestamp;effect.stream=p.stream;effectSession=p.sessionID;
                        std::copy_n(p.samples,p.frames*2,effect.samples);
                    }else{
                        if(size>sizeof(effect))continue;memcpy(&effect,buffer.data(),size);
                        if(effect.version!=1 || effect.frames>512 || size!=36+effect.frames*8)continue;
                    }
                    if(effect.frames<1 || effect.sampleRate<8000 || effect.sampleRate>192000 || effect.stream>1)continue;
                    bool finite=true;for(unsigned i=0;i<effect.frames*2;++i)finite&=std::isfinite(effect.samples[i]);if(!finite)continue;
                    std::lock_guard lock(mutex);
                    if(modernEffects){
                        if(effectSession!=session)continue;
                        if(!clock.valid()){
                            if(effect.stream==0){pendingEffects.push_back(effect);while(pendingEffects.size()>128)pendingEffects.pop_front();}
                            continue;
                        }
                        if(!clock.translate(effect.timestamp))continue;
                    }
#if defined(__linux__)
                    else if(effectsClock.translate(effect.timestamp,now))effects.clear();
#endif
                    if(effect.timestamp>now+1000000000 || effect.timestamp+1000000000<now)continue;
                    if(effect.stream==1){if(reference){std::fwrite(&effect,sizeof(effect),1,reference);std::fflush(reference);}continue;}
                    effects.push_back(effect);while(effects.size()>128)effects.pop_front();continue;
                }
                SongLinkPacket p{};
                if(size!=sizeof(p) && size!=SONG_LINK_V4_SIZE && size!=SONG_LINK_V3_SIZE && size!=SONG_LINK_V2_SIZE)continue;
                memcpy(&p,buffer.data(),size);
                if(!((size==sizeof(p)&&p.version==5)||(size==SONG_LINK_V4_SIZE&&p.version==4)||(size==SONG_LINK_V3_SIZE&&p.version==3)||(size==SONG_LINK_V2_SIZE&&p.version==2)) || memcmp(p.magic,"GDSONG1",8))continue;
                if(!std::isfinite(p.musicVolume)||p.musicVolume<0||p.musicVolume>1 || !std::isfinite(p.effectsVolume)||p.effectsVolume<0||p.effectsVolume>1 ||
                   !std::isfinite(p.position)||p.position< -600||p.position>=86400 || !std::isfinite(p.offset) || !std::isfinite(p.rate)||p.rate<.25||p.rate>4 ||
                   p.channelID< -1||p.channelID>=SONG_LINK_MAX_CHANNELS || !std::isfinite(p.triggerGain)||p.triggerGain<0||p.triggerGain>16 || p.fadeCount>8)continue;
                bool valid=true;uint64_t previous=0;
                for(unsigned i=0;i<p.fadeCount;++i){const auto& f=p.fades[i];if(f.offsetNs<=previous||f.offsetNs>86400000000000ULL||!std::isfinite(f.gain)||f.gain<0||f.gain>16)valid=false;previous=f.offsetNs;}
                if((p.flags&4) && (!std::isfinite(p.loopStart)||!std::isfinite(p.loopEnd)||p.loopStart<0||p.loopEnd<=p.loopStart||p.loopEnd>86400))valid=false;
                if(!valid)continue;
                p.status[23]=p.level[95]=p.song[95]=p.path[1023]=0;
                if(p.version<4)p.timestamp=now;
                std::lock_guard lock(mutex);
                if(p.version>=5){
                    if(!p.sessionID)continue;
                    if(session!=p.sessionID){session=p.sessionID;peer=from;clock.reset();pending.clear();pendingEffects.clear();probes.clear();lastProbe=0;clearTimeline();}
                    if(from.sin_port!=peer.sin_port)continue;
                    if(!clock.valid()){pending.push_back(p);while(pending.size()>256)pending.pop_front();probe(now);continue;}
                }else if(session)continue;
                accept(p,now);
            }
            if(reference)std::fclose(reference);
        });
    }
    ~Receiver(){stop=true;if(worker.joinable())worker.join();if(sock!=BAD_SOCKET)closeSocket(sock);}
    LinkState get(){std::lock_guard lock(mutex);return state;}
    std::vector<int> getChannels(){std::lock_guard lock(mutex);std::vector<int> ids;for(int i=0;i<=SONG_LINK_MAX_CHANNELS;++i)if(!playback[i].empty())ids.push_back(i-1);return ids;}
    std::vector<SongLinkPacket> playbackWindow(uint64_t start,uint64_t end,int channel=0){
        std::lock_guard lock(mutex);std::vector<SongLinkPacket> window(1);window.front().channelID=channel;
        if(channel< -1 || channel>=SONG_LINK_MAX_CHANNELS)return window;
        auto& voice=playback[channel+1];
        auto next=std::upper_bound(voice.begin(),voice.end(),start,[](uint64_t t,const auto& p){return t<p.timestamp;});
        if(next!=voice.begin())window.front()=*std::prev(next);
        for(;next!=voice.end()&&next->timestamp<end;++next)window.push_back(*next);
        return window;
    }
    void mixEffects(float* output,size_t frames,uint64_t start,float gain){
        std::lock_guard lock(mutex);
        for(const auto& p:effects){
            double delta=(double(start)-double(p.timestamp))/1e9;
            if(delta>double(p.frames)/p.sampleRate || delta+double(frames)/48000<0)continue;
            for(size_t i=0;i<frames;++i){double at=(delta+double(i)/48000)*p.sampleRate;if(at<0||at>=p.frames)continue;
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
    struct Voice {
        Decoder decoder;
        std::string loadedPath,resolvedPath;
        unsigned lastEpoch=~0u;
        bool wasPlaying=false,fileAvailable=false;
        uintmax_t loadedSize=0;
        std::filesystem::file_time_type loadedWriteTime{};
        Clock::time_point retryAt{};
    };
    void run(){
        std::array<std::unique_ptr<Voice>,SONG_LINK_MAX_CHANNELS> voices;
#if defined(__linux__)
        LinuxPaths paths;
#endif
        std::array<float,960> audio{},voiceAudio{};
        auto next=Clock::now();
        constexpr uint64_t bufferingNs=40000000;
        uint64_t timestamp=os_gettime_ns()-bufferingNs;
        while(!stop){
            next+=std::chrono::milliseconds(10);audio.fill(0);
            auto link=receiver->get();auto latest=link.packet;
            bool connected=link.connected && Clock::now()-link.received<std::chrono::milliseconds(400);
            auto channels=receiver->getChannels();
            auto frameAt=[&](uint64_t at){return at<=timestamp?size_t(0):size_t(std::min<uint64_t>(480,((at-timestamp)*48000+999999999)/1000000000));};
            auto segments=[&](int channel,auto render){
                auto window=receiver->playbackWindow(timestamp,timestamp+10000000,channel);
                for(size_t i=0;i<window.size();++i){
                    auto& p=window[i];size_t first=i?frameAt(p.timestamp):0;
                    size_t end=i+1<window.size()?frameAt(window[i+1].timestamp):480;
                    if(p.timestamp && timestamp+first*1000000000/48000>=p.timestamp+400000000)continue;
                    end=std::min(end,frameAt(p.timestamp+400000000));
                    if(end>first && p.timestamp)render(p,first,end,timestamp+first*1000000000/48000);
                }
            };
            for(int channel:channels){
                if(channel<0)continue;
                if(!voices[channel])voices[channel]=std::make_unique<Voice>();
                auto& v=*voices[channel];auto& decoder=v.decoder;
                segments(channel,[&](const SongLinkPacket& p,size_t first,size_t end,uint64_t segmentTime){
                    std::string newPath=p.path;bool pathChanged=newPath!=v.loadedPath;
                    if(pathChanged || (!newPath.empty() && Clock::now()>=v.retryAt)){
                        std::string resolved=newPath;
#if defined(__linux__)
                        resolved=paths.resolve(newPath);
#endif
                        std::error_code error;auto file=std::filesystem::u8path(resolved);
                        auto size=resolved.empty()?0:std::filesystem::file_size(file,error);
                        auto modified=resolved.empty()||error?std::filesystem::file_time_type{}:std::filesystem::last_write_time(file,error);
                        bool available=!resolved.empty()&&!error;
                        bool fileChanged=resolved!=v.resolvedPath||available!=v.fileAvailable||(available&&(size!=v.loadedSize||modified!=v.loadedWriteTime));
                        if(pathChanged||fileChanged||!decoder.ready()){
                            decoder.open(resolved);v.loadedPath=newPath;v.lastEpoch=~0u;
                            if(pathChanged||fileChanged||decoder.ready())blog(LOG_INFO,"[OBS Jukebox] Decoder %s: %s",decoder.ready()?"ready":"unavailable",p.song);
                        }
                        v.resolvedPath=resolved;v.fileAvailable=available;v.loadedSize=size;v.loadedWriteTime=modified;
                        v.retryAt=Clock::now()+std::chrono::seconds(1);
                    }
                    double age=double(int64_t(segmentTime)-int64_t(p.timestamp))/1e9;
                    double sourcePosition=p.position+((p.flags&2)?age*p.rate:0);
                    bool looping=(p.flags&4)!=0;
                    if(looping && sourcePosition>=p.loopEnd)sourcePosition=p.loopStart+std::fmod(sourcePosition-p.loopStart,p.loopEnd-p.loopStart);
                    double target=sourcePosition+p.offset;
                    bool playing=(p.flags&3)==3 && active && std::isfinite(target)&&target>=0&&target<=31536000.0&&decoder.ready();
                    voiceAudio.fill(0);
                    if(playing){
                        if(v.lastEpoch!=p.epoch||!v.wasPlaying||std::abs(decoder.position-target)>.04)playing=decoder.seek(target);
                        if(playing){
                            size_t rendered=0;
                            while(rendered<end-first){
                                size_t count=end-first-rendered;
                                if(looping){
                                    double untilEnd=(p.loopEnd-sourcePosition)*48000/p.rate;
                                    count=std::min(count,size_t(std::max(1.0,std::ceil(untilEnd))));
                                }
                                decoder.render(voiceAudio.data()+rendered*2,count,p.rate);
                                rendered+=count;sourcePosition+=double(count)*p.rate/48000;
                                if(looping && sourcePosition>=p.loopEnd){
                                    sourcePosition=p.loopStart+std::fmod(sourcePosition-p.loopStart,p.loopEnd-p.loopStart);
                                    if(!decoder.seek(sourcePosition+p.offset)){playing=false;break;}
                                }
                            }
                        }
                    }
                    for(size_t j=0;j<end-first;++j){
                        float gain=p.musicVolume*songGainAt(p,segmentTime+j*1000000000/48000);
                        for(size_t ch=0;ch<2;++ch)audio[(first+j)*2+ch]+=voiceAudio[j*2+ch]*gain;
                    }
                    v.wasPlaying=playing;v.lastEpoch=p.epoch;
                });
            }
            int global=std::find(channels.begin(),channels.end(),-1)!=channels.end()?-1:0;
            if(active)segments(global,[&](const SongLinkPacket& p,size_t first,size_t end,uint64_t at){
                if(p.flags&1)receiver->mixEffects(audio.data()+first*2,end-first,at,p.effectsVolume);
            });
            {
                std::lock_guard lock(mutex);
                detail=!connected?"Waiting for Geometry Dash":!latest.song[0]?"Select the OBS checkbox beside a Jukebox song.":
                    std::string(latest.song)+" | "+latest.status+" | Attempt "+std::to_string(latest.attempt);
            }
            obs_source_audio block{};block.data[0]=reinterpret_cast<const uint8_t*>(audio.data());block.frames=480;
            block.speakers=SPEAKERS_STEREO;block.format=AUDIO_FORMAT_FLOAT;block.samples_per_sec=48000;
            block.timestamp=timestamp;obs_source_output_audio(source,&block);timestamp+=10000000;
            auto now=Clock::now();if(next<now-std::chrono::milliseconds(100)){next=now;timestamp=os_gettime_ns()-bufferingNs;}
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
