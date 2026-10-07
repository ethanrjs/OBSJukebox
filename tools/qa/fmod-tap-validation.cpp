#include <windows.h>
#include "../geode-sdk/loader/include/Geode/fmod/fmod.h"
#include "../geode-sdk/loader/include/Geode/fmod/fmod_dsp.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <atomic>
#include <algorithm>
struct Probe {
    std::atomic<int>* phase=nullptr;
    double energy[3]{}; unsigned long long samples[3]{};
    unsigned calls=0,audible=0,gaps=0,resets=0; int rate=0;
    unsigned long long lastClock=0,frames=0,start=0,lastWall=0;
    unsigned lastFrames=0; double minAge=1e9,maxAge=-1e9,maxGap=0;
};
static unsigned long long now(){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return t.QuadPart/f.QuadPart*1000000000ull+t.QuadPart%f.QuadPart*1000000000ull/f.QuadPart;}
static FMOD_RESULT F_CALL render(FMOD_DSP_STATE* s,float* in,float* out,unsigned n,int channels,int* outChannels){
    void* u=nullptr;s->functions->getuserdata(s,&u);auto& p=*static_cast<Probe*>(u);
    s->functions->getsamplerate(s,&p.rate);unsigned long long clock=0;unsigned offset=0,len=0;s->functions->getclock(s,&clock,&offset,&len);
    auto wall=now();auto stamp=p.start+p.frames*1000000000ull/p.rate;
    if(!p.start || wall>stamp+20000000 || stamp>wall+100000000){p.start=stamp=wall;p.frames=0;++p.resets;}
    if(p.calls && clock!=p.lastClock+p.lastFrames)++p.gaps;
    if(p.lastWall)p.maxGap=std::max(p.maxGap,double(wall-p.lastWall)/1e6);
    bool sound=false;for(unsigned i=0;i<n*channels;++i)sound|=std::abs(in[i])>1e-5;
    auto phase=p.phase?p.phase->load():-1;
    if(phase>=0){for(unsigned i=0;i<n*channels;++i)p.energy[phase]+=double(in[i])*in[i];p.samples[phase]+=n*channels;}
    if(sound){++p.audible;auto age=(double(wall)-double(stamp))/1e6;p.minAge=std::min(p.minAge,age);p.maxAge=std::max(p.maxAge,age);}
    ++p.calls;p.lastClock=clock;p.lastFrames=n;p.lastWall=wall;p.frames+=n;
    *outChannels=channels;if(in!=out)std::memcpy(out,in,n*channels*sizeof(float));return FMOD_OK;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3){std::puts("usage: fmod-tap-validation fmod.dll tone.wav");return 2;}
    auto lib=LoadLibraryW(argv[1]);if(!lib)return 3;
#define LOAD(name) auto name=reinterpret_cast<decltype(&::FMOD_##name)>(GetProcAddress(lib,"FMOD_" #name));if(!name)return 4
    LOAD(System_Create);LOAD(System_SetOutput);LOAD(System_Init);LOAD(System_CreateChannelGroup);LOAD(System_CreateDSP);LOAD(ChannelGroup_AddDSP);LOAD(System_CreateSound);LOAD(System_PlaySound);LOAD(Channel_Stop);LOAD(System_Update);LOAD(System_Release);LOAD(System_GetMasterChannelGroup);LOAD(ChannelGroup_AddGroup);
    LOAD(ChannelGroup_GetDSP);LOAD(ChannelGroup_GetDSPIndex);LOAD(ChannelGroup_SetVolume);
#define CHECK(call) if(auto err=(call)){std::printf("%s failed: %d\n",#call,err);return 5;}
    FMOD_SYSTEM* system=nullptr;CHECK(System_Create(&system,FMOD_VERSION));CHECK(System_SetOutput(system,FMOD_OUTPUTTYPE_NOSOUND));CHECK(System_Init(system,64,FMOD_INIT_NORMAL,nullptr));
    FMOD_CHANNELGROUP *master=nullptr,*group=nullptr;CHECK(System_GetMasterChannelGroup(system,&master));CHECK(System_CreateChannelGroup(system,"effects",&group));CHECK(ChannelGroup_AddGroup(master,group,true,nullptr));
    std::atomic<int> phase{-1};Probe p,local;p.phase=local.phase=&phase;
    FMOD_DSP_DESCRIPTION d{};d.pluginsdkversion=FMOD_PLUGIN_SDK_VERSION;std::strcpy(d.name,"probe");d.numinputbuffers=d.numoutputbuffers=1;d.read=render;d.userdata=&p;FMOD_DSP* dsp=nullptr;CHECK(System_CreateDSP(system,&d,&dsp));
    FMOD_DSP* fader=nullptr;int index=0;CHECK(ChannelGroup_GetDSP(group,FMOD_CHANNELCONTROL_DSP_FADER,&fader));CHECK(ChannelGroup_GetDSPIndex(group,fader,&index));CHECK(ChannelGroup_AddDSP(group,index+1,dsp));
    d.userdata=&local;FMOD_DSP* output=nullptr;CHECK(System_CreateDSP(system,&d,&output));CHECK(ChannelGroup_AddDSP(group,FMOD_CHANNELCONTROL_DSP_HEAD,output));
    char path[4096];WideCharToMultiByte(CP_UTF8,0,argv[2],-1,path,sizeof(path),nullptr,nullptr);FMOD_SOUND* sound=nullptr;CHECK(System_CreateSound(system,path,FMOD_DEFAULT,nullptr,&sound));
    auto wait=[&](int ms){for(int i=0;i<ms;i+=5){System_Update(system);Sleep(5);}};
    wait(300);std::printf("idle callbacks: %u\n",p.calls);
    for(int i=0;i<20;++i){FMOD_CHANNEL* ch=nullptr;CHECK(System_PlaySound(system,sound,group,false,&ch));wait(30);Channel_Stop(ch);wait(60);}
    wait(300);
    const float volumes[]={1.0f,.25f,0.0f};
    for(int i=0;i<3;++i){
        CHECK(ChannelGroup_SetVolume(group,volumes[i]));FMOD_CHANNEL* ch=nullptr;CHECK(System_PlaySound(system,sound,group,false,&ch));
        wait(150);phase.store(i);wait(350);phase.store(-1);CHECK(Channel_Stop(ch));wait(100);
    }
    CHECK(System_Release(system));
    std::printf("calls=%u audible=%u rate=%d clock_gaps=%u resets=%u max_callback_gap_ms=%.3f audible_age_ms=[%.3f, %.3f]\n",p.calls,p.audible,p.rate,p.gaps,p.resets,p.maxGap,p.minAge,p.maxAge);
    bool pass=p.audible>0;
    const auto baseline=p.samples[0]?std::sqrt(p.energy[0]/p.samples[0]):0.;
    for(int i=0;i<3;++i){
        auto captured=p.samples[i]?std::sqrt(p.energy[i]/p.samples[i]):0.;
        auto played=local.samples[i]?std::sqrt(local.energy[i]/local.samples[i]):0.;
        auto ratio=captured?played/captured:0.;
        std::printf("game_sfx=%.2f captured_rms=%.6f local_rms=%.6f local_ratio=%.6f\n",volumes[i],captured,played,ratio);
        pass&=captured>1e-4 && baseline>0 && std::abs(captured/baseline-1.)<.01 && std::abs(ratio-volumes[i])<.01;
    }
    return pass?0:1;
}
