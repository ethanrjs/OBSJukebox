#include <windows.h>
#include "../geode-sdk/loader/include/Geode/fmod/fmod.h"
#include "../geode-sdk/loader/include/Geode/fmod/fmod_dsp.h"
#include <atomic>
#include <cstdio>
#include <cstring>
// GD's layout: music and effects are sibling groups under the master group, and the effects tap
// sits on the effects group. FMOD stops a group's DSP clock while the group is paused, and GD
// pauses the two groups at different times, so the mod dates music positions with the effects
// group's clock. This checks that clock stays on the tap's timeline after either group is paused
// alone, and that the music channel's parent clock does not.
static std::atomic<unsigned long long> tapClock{0};
static FMOD_RESULT F_CALL tap(FMOD_DSP_STATE* s,float* in,float* out,unsigned n,int channels,int* outChannels){
    unsigned long long clock=0;unsigned offset=0,length=0;
    if(s->functions->getclock(s,&clock,&offset,&length)==FMOD_OK)tapClock=clock;
    *outChannels=channels;if(in!=out)std::memcpy(out,in,n*channels*sizeof(float));return FMOD_OK;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3){std::puts("usage: fmod-clock-validation fmod.dll tone.wav");return 2;}
    auto lib=LoadLibraryW(argv[1]);if(!lib)return 3;
#define LOAD(name) auto name=reinterpret_cast<decltype(&::FMOD_##name)>(GetProcAddress(lib,"FMOD_" #name));if(!name)return 4
    LOAD(System_Create);LOAD(System_SetOutput);LOAD(System_Init);LOAD(System_GetSoftwareFormat);LOAD(System_GetMasterChannelGroup);
    LOAD(System_CreateChannelGroup);LOAD(ChannelGroup_AddGroup);LOAD(System_CreateDSP);LOAD(ChannelGroup_AddDSP);LOAD(System_CreateSound);
    LOAD(System_PlaySound);LOAD(System_Update);LOAD(System_Release);LOAD(ChannelGroup_SetPaused);LOAD(ChannelGroup_GetDSPClock);LOAD(Channel_GetDSPClock);
#define CHECK(call) if(auto err=(call)){std::printf("%s failed: %d\n",#call,err);return 5;}
    FMOD_SYSTEM* system=nullptr;CHECK(System_Create(&system,FMOD_VERSION));CHECK(System_SetOutput(system,FMOD_OUTPUTTYPE_NOSOUND));CHECK(System_Init(system,64,FMOD_INIT_NORMAL,nullptr));
    int rate=0;CHECK(System_GetSoftwareFormat(system,&rate,nullptr,nullptr));
    FMOD_CHANNELGROUP *master=nullptr,*effects=nullptr,*music=nullptr;CHECK(System_GetMasterChannelGroup(system,&master));
    CHECK(System_CreateChannelGroup(system,"effects",&effects));CHECK(ChannelGroup_AddGroup(master,effects,true,nullptr));
    CHECK(System_CreateChannelGroup(system,"music",&music));CHECK(ChannelGroup_AddGroup(master,music,true,nullptr));
    FMOD_DSP_DESCRIPTION d{};d.pluginsdkversion=FMOD_PLUGIN_SDK_VERSION;std::strcpy(d.name,"tap");d.numinputbuffers=d.numoutputbuffers=1;d.read=tap;
    FMOD_DSP* dsp=nullptr;CHECK(System_CreateDSP(system,&d,&dsp));CHECK(ChannelGroup_AddDSP(effects,FMOD_CHANNELCONTROL_DSP_TAIL,dsp));
    char path[4096];WideCharToMultiByte(CP_UTF8,0,argv[2],-1,path,sizeof(path),nullptr,nullptr);
    FMOD_SOUND* sound=nullptr;CHECK(System_CreateSound(system,path,FMOD_LOOP_NORMAL,nullptr,&sound));
    FMOD_CHANNEL *song=nullptr,*sfx=nullptr;CHECK(System_PlaySound(system,sound,music,false,&song));CHECK(System_PlaySound(system,sound,effects,false,&sfx));
    auto wait=[&](int ms){for(int i=0;i<ms;i+=5){System_Update(system);Sleep(5);}};
    // Main-thread reads trail the tap's latest block by up to a few blocks; drift is whole pauses.
    const double tolerance=100;bool pass=true;
    auto check=[&](const char* label,bool drifted){
        double block=double(tapClock.load());
        unsigned long long group=0,parent=0;ChannelGroup_GetDSPClock(effects,&group,nullptr);Channel_GetDSPClock(song,nullptr,&parent);
        double effectsError=(double(group)-block)*1000/rate,parentError=(double(parent)-block)*1000/rate;
        bool ok=effectsError>=0 && effectsError<tolerance && (drifted ? parentError>=tolerance || parentError<0 : parentError>=0 && parentError<tolerance);
        std::printf("%-24s effects group clock %+8.1f ms, music parent clock %+8.1f ms from the tap: %s\n",label,effectsError,parentError,ok?"ok":"FAIL");
        pass&=ok;
    };
    wait(300);check("both playing",false);
    CHECK(ChannelGroup_SetPaused(effects,true));wait(500);CHECK(ChannelGroup_SetPaused(effects,false));wait(200);check("effects paused alone",true);
    CHECK(ChannelGroup_SetPaused(music,true));wait(1000);CHECK(ChannelGroup_SetPaused(music,false));wait(200);check("music paused alone",true);
    CHECK(ChannelGroup_SetPaused(effects,true));CHECK(ChannelGroup_SetPaused(music,true));wait(500);
    CHECK(ChannelGroup_SetPaused(effects,false));CHECK(ChannelGroup_SetPaused(music,false));wait(200);check("both paused together",true);
    CHECK(System_Release(system));
    return pass?0:1;
}
