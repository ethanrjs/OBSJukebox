#include "Socket.hpp"
#include "AudioTap.hpp"
#include "Bridge.hpp"
#include "LinkPacket.hpp"
#include "MonotonicClock.hpp"
#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod_dsp.h>
#include <atomic>
#include <array>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdlib>
using namespace geode::prelude;
namespace separate_song::audio_tap {
namespace {
struct Tap {
    static constexpr unsigned capacity=64;
    std::array<EffectsPacketV2,capacity> queue;
    std::atomic<unsigned> read{0},write{0};
    std::atomic<bool> enabled{true};
    uint32_t rate=44100, sequence=0, stream=0;
    uint64_t clockStart=0, clockFrames=0;
    FMOD::DSP* dsp=nullptr;
    void push(float* input,unsigned frames,int channels) {
        if(!enabled.load(std::memory_order_relaxed) || channels<1 || frames==0)return;
        auto wall=monotonicNowNs();
        auto timestamp=clockStart+clockFrames*1000000000/rate;
        if(!clockStart || wall>timestamp+20000000 || timestamp>wall+100000000){
            clockStart=wall;clockFrames=0;timestamp=wall;
        }
        for(unsigned start=0;start<frames;start+=512){
            auto w=write.load(std::memory_order_relaxed);
            if(w-read.load(std::memory_order_acquire)>=capacity)break;
            auto& p=queue[w%capacity];p.frames=std::min(512u,frames-start);
            p.sequence=sequence++;p.sampleRate=rate;p.stream=stream;p.sessionID=processSessionID();
            p.timestamp=clockStart+(clockFrames+start)*1000000000/rate;
            for(unsigned i=0;i<p.frames;++i){
                p.samples[i*2]=input[(start+i)*channels];
                p.samples[i*2+1]=input[(start+i)*channels+(channels>1?1:0)];
            }
            write.store(w+1,std::memory_order_release);
        }
        clockFrames+=frames;
    }
    static FMOD_RESULT F_CALL render(FMOD_DSP_STATE* state,float* input,float* output,unsigned frames,int channels,int* outChannels) {
        *outChannels=channels;
        void* user=nullptr;state->functions->getuserdata(state,&user);
        if(user && input)static_cast<Tap*>(user)->push(input,frames,channels);
        if(input && output){
            if(user && static_cast<Tap*>(user)->stream==2)std::memset(output,0,frames*channels*sizeof(float));
            else if(input!=output)std::memcpy(output,input,frames*channels*sizeof(float));
        }
        return FMOD_OK;
    }
    bool attach(FMODAudioEngine* engine,FMOD::ChannelGroup* group,uint32_t id){
        if(!group || !engine->m_system)return false;
        stream=id;rate=engine->m_sampleRate;
        FMOD_DSP_DESCRIPTION description{};
        description.pluginsdkversion=FMOD_PLUGIN_SDK_VERSION;
        std::strncpy(description.name,id==2?"OBS replacement":id?"OBS calibration":"OBS effects",31);
        description.version=0x10000;description.numinputbuffers=1;description.numoutputbuffers=1;
        description.read=render;description.userdata=this;
        if(engine->m_system->createDSP(&description,&dsp)!=FMOD_OK)return false;
        int position=FMOD_CHANNELCONTROL_DSP_HEAD;
        if(id==0){
            FMOD::DSP* fader=nullptr;
            if(group->getDSP(FMOD_CHANNELCONTROL_DSP_FADER,&fader)!=FMOD_OK ||
               group->getDSPIndex(fader,&position)!=FMOD_OK){dsp->release();dsp=nullptr;return false;}
            ++position;
        }
        if(group->addDSP(position,dsp)!=FMOD_OK){dsp->release();dsp=nullptr;return false;}
        std::thread([this]{
            if(!socketsReady())return;
            auto socket=::socket(AF_INET,SOCK_DGRAM,0);if(socket==BAD_SOCKET)return;
            nonblocking(socket);
            sockaddr_in target{};target.sin_family=AF_INET;target.sin_port=htons(receiverPort());
            target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
            for(;;){
                auto r=read.load(std::memory_order_relaxed);
                while(r!=write.load(std::memory_order_acquire)){
                    auto& p=queue[r%capacity];
                    sendto(socket,reinterpret_cast<const char*>(&p),44+p.frames*8,0,reinterpret_cast<sockaddr*>(&target),sizeof(target));
                    read.store(++r,std::memory_order_release);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }).detach();
        return true;
    }
};
Tap* effects=nullptr;
Tap* reference=nullptr;
bool attachFailureLogged=false;

}
void install(){
    if(effects)return;
    auto engine=FMODAudioEngine::get();if(!engine || !engine->m_globalChannel)return;
    auto tap=new Tap;
    if(!tap->attach(engine,engine->m_globalChannel,0)){
        delete tap;
        if(!attachFailureLogged){attachFailureLogged=true;log::error("OBS effects tap could not attach");}
        return;
    }
    effects=tap;log::info("OBS effects tap attached at {} Hz",tap->rate);
    if(std::getenv("SEPARATE_SONG_CALIBRATE")){
        engine->setBackgroundMusicVolume(.5f);
        engine->setEffectsVolume(.5f);
        auto ref=new Tap;
        if(ref->attach(engine,engine->m_backgroundMusicChannel,1))reference=ref;else delete ref;
    }
}
void enable(bool value){if(effects)effects->enabled=value;}
}
