#include "../../src/Socket.hpp"
#include "../../src/LinkPacket.hpp"
#include <obs.h>
#include <util/platform.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
struct Capture {
    std::mutex mutex;
    std::vector<float> samples;
    uint64_t first=0,last=0;size_t mutedFrames=0;
    static void callback(void* param, obs_source_t*, const audio_data* a, bool muted) {
        auto& c=*static_cast<Capture*>(param); std::lock_guard lock(c.mutex);
        auto f=reinterpret_cast<const float*>(a->data[0]);
        if(f) c.samples.insert(c.samples.end(),f,f+a->frames);
        if(muted)c.mutedFrames+=a->frames;
        if(!c.first)c.first=a->timestamp;c.last=a->timestamp;
    }
    void reset(){std::lock_guard lock(mutex);samples.clear();first=last=0;mutedFrames=0;}
};
struct Metric {size_t frames=0;double rms=0,hz=0,cpu=0,mutedFraction=0,mean=0;bool pass=false;};
uint64_t cpuTicks(){
#ifdef _WIN32
    FILETIME a,b,k,u;GetProcessTimes(GetCurrentProcess(),&a,&b,&k,&u);ULARGE_INTEGER kk,uu;kk.LowPart=k.dwLowDateTime;kk.HighPart=k.dwHighDateTime;uu.LowPart=u.dwLowDateTime;uu.HighPart=u.dwHighDateTime;return kk.QuadPart+uu.QuadPart;
#else
    rusage usage{};getrusage(RUSAGE_SELF,&usage);return uint64_t(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*10000000+uint64_t(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*10;
#endif
}
void wave(const std::filesystem::path& path,double frequencyScale=1,uint32_t samples=48000*6) {
    std::ofstream out(path,std::ios::binary);uint32_t size=samples*4;
    auto put=[&](auto n){out.write(reinterpret_cast<const char*>(&n),sizeof(n));};
    out.write("RIFF",4);put(size+36);out.write("WAVEfmt ",8);put(uint32_t(16));put(uint16_t(1));put(uint16_t(2));put(uint32_t(48000));put(uint32_t(192000));put(uint16_t(4));put(uint16_t(16));out.write("data",4);put(size);
    for(uint32_t i=0;i<samples;++i){double hz=(i<48000?440:i<96000?660:i<192000?880:1100)*frequencyScale;int16_t v=int16_t(std::sin(2*3.141592653589793*hz*i/48000)*9000);put(v);put(v);}
    out.close();
    std::error_code error;auto written=std::filesystem::file_size(path,error);
    if(!out || error || written!=size+44){fprintf(stderr,"WAV fixture write failed: %s\n",path.string().c_str());std::exit(2);}
}
int main(int argc,char** argv){
    if(argc<3){fprintf(stderr,"Usage: harness plugin-path output-directory [optional-mp3 | --live [seconds]]\n");return 2;}
    bool live=argc>3 && !std::strcmp(argv[3],"--live");
    double liveSeconds=live&&argc>4?std::strtod(argv[4],nullptr):15;
    if(live && (!std::isfinite(liveSeconds)||liveSeconds<1||liveSeconds>60)){
        fprintf(stderr,"Live capture requires 1-60 seconds\n");return 2;
    }

    if(!socketsReady()){fprintf(stderr,"sockets unavailable\n");return 2;}
    auto preflight=socket(AF_INET,SOCK_DGRAM,0);sockaddr_in preflightAddress{};
    preflightAddress.sin_family=AF_INET;preflightAddress.sin_port=htons(receiverPort());preflightAddress.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(preflight==BAD_SOCKET||bind(preflight,reinterpret_cast<sockaddr*>(&preflightAddress),sizeof(preflightAddress))!=0){
        if(preflight!=BAD_SOCKET)closeSocket(preflight);fprintf(stderr,"UDP receiver port is already bound; refusing to compete with another receiver\n");return 2;
    }
    closeSocket(preflight);
    std::filesystem::path output=std::filesystem::u8path(argv[2]);std::filesystem::create_directories(output);
    auto wav=output/std::filesystem::path(u8"runtime-曲-é.wav");if(!live)wave(wav);auto utf8=wav.u8string();
    if(!obs_startup("en-US",nullptr,nullptr)){fprintf(stderr,"obs_startup failed\n");return 2;}

#ifdef _WIN32
    const auto obsDirectory = std::getenv("OBS_QA_DIRECTORY");
    if (!obsDirectory || !*obsDirectory) {
        fprintf(stderr,"Set OBS_QA_DIRECTORY to the installed OBS directory (the PowerShell runner does this).\n");
        obs_shutdown();
        return 2;
    }
    auto obsRoot = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(obsDirectory)));
    auto dataPath = (obsRoot / "data/libobs").generic_string() + "/";
    auto graphicsPath = (obsRoot / "bin/64bit/libobs-d3d11.dll").generic_string();
    obs_add_data_path(dataPath.c_str());
    const char* graphicsModule=graphicsPath.c_str();
#elif defined(__APPLE__)
    obs_add_data_path(OBS_QA_DATA_PATH);
    const char* graphicsModule=OBS_QA_GRAPHICS_MODULE;
#else
    const char* graphicsModule="libobs-opengl.so";
#endif
    obs_video_info vi{};vi.graphics_module=graphicsModule;vi.fps_num=30;vi.fps_den=1;
    vi.base_width=vi.output_width=128;vi.base_height=vi.output_height=128;
    vi.output_format=VIDEO_FORMAT_RGBA;vi.colorspace=VIDEO_CS_709;vi.range=VIDEO_RANGE_FULL;vi.scale_type=OBS_SCALE_BILINEAR;
    if(obs_reset_video(&vi)!=OBS_VIDEO_SUCCESS){fprintf(stderr,"video tick initialization failed\n");obs_shutdown();return 2;}
    obs_audio_info ai{48000,SPEAKERS_STEREO};if(!obs_reset_audio(&ai)){fprintf(stderr,"audio reset failed\n");return 2;}
    obs_module_t* module=nullptr;int opened=obs_open_module(&module,argv[1],argv[2]);
    if(opened!=0 || !obs_init_module(module)){fprintf(stderr,"module failed %d\n",opened);return 2;}
    auto settings=obs_data_create();auto source=obs_source_create("gd_alternate_song","Runtime validation GD Sounds",settings,nullptr);obs_data_release(settings);
    if(!source){fprintf(stderr,"GD Sounds source failed\n");return 2;}
    Capture capture;obs_source_add_audio_capture_callback(source,Capture::callback,&capture);obs_source_inc_active(source);
    if(live){
        printf("LIVE capture: %.1f seconds of actual GD packets on port %d%s. No synthetic packets are sent.\n",liveSeconds,receiverPort(),receiverPort()==SONG_LINK_PORT?"":" (explicit relay port)");fflush(stdout);
        std::this_thread::sleep_for(std::chrono::duration<double>(liveSeconds));
        obs_source_remove_audio_capture_callback(source,Capture::callback,&capture);
        double squares=0,peak=0;size_t nonzero=0,invalid=0;
        for(auto sample:capture.samples){if(!std::isfinite(sample)){++invalid;continue;}squares+=double(sample)*sample;peak=std::max(peak,double(std::abs(sample)));if(std::abs(sample)>1e-5)++nonzero;}
        auto frames=capture.samples.size();auto rms=frames?std::sqrt(squares/frames):0;

        std::ofstream audio(output/"live-source-left.wav",std::ios::binary);
        auto put=[&](auto n){audio.write(reinterpret_cast<const char*>(&n),sizeof(n));};uint32_t bytes=uint32_t(frames*sizeof(float));
        audio.write("RIFF",4);put(bytes+36);audio.write("WAVEfmt ",8);put(uint32_t(16));put(uint16_t(3));put(uint16_t(1));put(uint32_t(48000));put(uint32_t(192000));put(uint16_t(4));put(uint16_t(32));audio.write("data",4);put(bytes);audio.write(reinterpret_cast<const char*>(capture.samples.data()),bytes);audio.close();
        std::ofstream json(output/"live-source-results.json");
        json<<"{\"scope\":\"Native OBS source left-channel PCM from live GD packets; no synthetic sender; an explicit relay port may strip music paths for effects-only validation; not physical listening\",\"receiver_port\":"<<receiverPort()<<",\"observed_seconds\":"<<liveSeconds<<",\"frames\":"<<frames<<",\"nonzero_frames\":"<<nonzero<<",\"invalid_samples\":"<<invalid<<",\"rms\":"<<rms<<",\"peak\":"<<peak<<",\"muted_frames\":"<<capture.mutedFrames<<",\"first_timestamp\":"<<capture.first<<",\"last_timestamp\":"<<capture.last<<",\"audio_present\":"<<(nonzero?"true":"false")<<"}\n";
        printf("LIVE frames=%zu nonzero=%zu invalid=%zu rms=%.6f peak=%.6f muted=%zu\n",frames,nonzero,invalid,rms,peak,capture.mutedFrames);
        obs_source_dec_active(source);obs_source_release(source);obs_shutdown();return frames&&invalid==0?0:1;
    }
    socketsReady();auto sock=socket(AF_INET,SOCK_DGRAM,0);sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    nonblocking(sock);
    std::atomic<bool> finishClock{false};
    std::thread clockResponder([&]{
        while(!finishClock){
            ClockSyncPacket reply{};sockaddr_in from{};
#ifdef _WIN32
            int length=sizeof(from);
#else
            socklen_t length=sizeof(from);
#endif
            auto size=recvfrom(sock,reinterpret_cast<char*>(&reply),sizeof(reply),0,reinterpret_cast<sockaddr*>(&from),&length);
            if(size==sizeof(reply)&&!memcmp(reply.magic,"GDCLK1\0",8)&&reply.kind==1){
                reply.kind=2;reply.t2=os_gettime_ns();reply.t3=os_gettime_ns();
                sendto(sock,reinterpret_cast<const char*>(&reply),sizeof(reply),0,reinterpret_cast<sockaddr*>(&from),length);
            } else { waitSocketReadable(sock, 50); }
        }
    });
    SongLinkPacket packet;packet.version=3;std::memcpy(packet.path,utf8.data(),std::min(utf8.size(),sizeof(packet.path)-1));std::strcpy(packet.song,"Runtime synthetic tone");std::strcpy(packet.status,"playing");packet.flags=3;packet.epoch=1;
    std::ofstream json(output/"runtime-results.json");json<<"{\"host\":\"Installed OBS libobs\",\"plugin\":\""<<std::filesystem::path(argv[1]).filename().string()<<"\",\"cases\":[\n";bool first=true;int failures=0;
    auto phase=[&](const char* name,double seconds,double expected,double pos,uint32_t flags,double rate=1,double offset=0,bool transmit=true,int effects=0,bool expectedMuted=false,bool expectEffectMean=false,bool advanceEpoch=true,double settleSeconds=.55){
        if(std::getenv("OBS_QA_RECOVERY_ONLY") && std::strcmp(name,"mp3_decode") && std::strcmp(name,"failed_seek_music_silence") && std::strcmp(name,"failed_seek_effects_continue") && std::strcmp(name,"failed_seek_restart_recovery"))return Metric{};
        packet.position=pos;packet.flags=flags;packet.rate=rate;packet.offset=offset;if(advanceEpoch)packet.epoch++;
        auto start=Clock::now(),end=start+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));uint64_t cpu=cpuTicks();capture.reset();bool reset=false;
        uint64_t effectClock=os_gettime_ns(),effectFrames=0;uint32_t sequence=0;
        while(Clock::now()<end){double elapsed=std::chrono::duration<double>(Clock::now()-start).count();packet.position=pos+((flags&2)?elapsed*rate:0);
            packet.timestamp=os_gettime_ns();
            if(transmit)sendto(sock,reinterpret_cast<const char*>(&packet),packet.version==2?SONG_LINK_V2_SIZE:packet.version==3?SONG_LINK_V3_SIZE:packet.version==4?SONG_LINK_V4_SIZE:sizeof(packet),0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
            if(effects==1 && packet.version>=5){EffectsPacketV2 e;e.sessionID=packet.sessionID;e.frames=480;e.sampleRate=48000;e.timestamp=os_gettime_ns();for(unsigned i=0;i<e.frames*2;++i)e.samples[i]=.15f;sendto(sock,reinterpret_cast<const char*>(&e),44+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));}
            if(effects==1 && packet.version<5){EffectsPacket e;e.frames=480;e.sampleRate=48000;e.timestamp=os_gettime_ns();for(unsigned i=0;i<e.frames;++i)e.samples[i*2]=e.samples[i*2+1]=.15f;sendto(sock,reinterpret_cast<const char*>(&e),36+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));}
            if(effects==2){

                double jitter=(effectFrames/1024)%2?.006:0;
                while(elapsed>=double(effectFrames)/44100+jitter){
                    for(unsigned part=0;part<2;++part){EffectsPacket e;e.frames=512;e.sampleRate=44100;e.sequence=sequence++;e.timestamp=effectClock+effectFrames*1000000000/44100;effectFrames+=512;for(unsigned i=0;i<e.frames*2;++i)e.samples[i]=.15f;sendto(sock,reinterpret_cast<const char*>(&e),36+e.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));}
                    jitter=(effectFrames/1024)%2?.006:0;
                }
            }
            if(!reset&&elapsed>settleSeconds){capture.reset();reset=true;}std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        double cpuPercent=double(cpuTicks()-cpu)/1e7/seconds*100;
        Metric m;{std::lock_guard lock(capture.mutex);m.frames=capture.samples.size();double squares=0,total=0;size_t crosses=0;for(size_t i=0;i<m.frames;++i){double v=capture.samples[i];squares+=v*v;total+=v;if(i&&capture.samples[i-1]<=0&&v>0)crosses++;}if(m.frames){m.rms=std::sqrt(squares/m.frames);m.mean=total/m.frames;m.hz=double(crosses)*48000/m.frames;m.mutedFraction=double(capture.mutedFrames)/m.frames;}}
        m.pass=m.frames>1000&&(expected==0?m.rms<.00001:expected<0?m.rms>.05:m.rms>.05&&std::abs(m.hz-expected)<expected*.08)&&(expectedMuted?m.mutedFraction>.99:m.mutedFraction<.01)&&(!expectEffectMean||m.mean>.03);
        if(effects==2){std::lock_guard lock(capture.mutex);for(float sample:capture.samples)if(std::abs(sample-.15f*packet.effectsVolume)>.001f)m.pass=false;}
        if(packet.musicVolume==.5f && expected>0 && !effects)m.pass=m.pass&&m.rms>.09&&m.rms<.105;
        if(!m.pass)failures++;if(!first)json<<",\n";first=false;json<<"{\"name\":\""<<name<<"\",\"frames\":"<<m.frames<<",\"raw_rms\":"<<m.rms<<",\"sample_mean\":"<<m.mean<<",\"frequency_hz\":"<<m.hz<<",\"obs_callback_muted_fraction\":"<<m.mutedFraction<<",\"process_cpu_one_core_percent\":"<<cpuPercent<<",\"pass\":"<<(m.pass?"true":"false")<<"}";
        printf("%s: %s rms=%.5f hz=%.1f mean=%.4f muted=%.2f CPU=%.2f%% of one core\n",name,m.pass?"PASS":"FAIL",m.rms,m.hz,m.mean,m.mutedFraction,cpuPercent);fflush(stdout);return m;
    };
    phase("no_link_silence",1,0,0,0,1,0,false);
    phase("unicode_wav_decode",1,440,0,3);
    packet.musicVolume=0;phase("active_volume_mute_same_path_and_epoch",1,0,1,3,1,0,true,0,false,false,false);
    packet.musicVolume=1;phase("active_volume_restore_same_path_and_epoch",1,880,2,3,1,0,true,0,false,false,false);
    packet.version=2;phase("legacy_v2_default_volumes",1,440,0,3);packet.version=3;
    packet.musicVolume=0;phase("obs_music_muted",1,0,0,3);
    phase("obs_music_muted_effects_unaffected",1,-1,0,3,1,0,true,2);
    packet.musicVolume=.5f;phase("obs_music_half_gain",1,440,0,3);
    packet.musicVolume=1;packet.effectsVolume=0;
    phase("obs_effects_muted",1,0,0,1,1,0,true,2);
    phase("obs_effects_muted_music_unaffected",1,440,0,3,1,0,true,1);
    packet.effectsVolume=.5f;phase("obs_effects_half_gain",1,-1,0,1,1,0,true,2);
    packet.effectsVolume=1;
    phase("pause_silence",1,0,.3,1);
    phase("resume",1,440,0,3);
    phase("pause_before_song_switch",1,0,2,1,1,0,true,0,false,false,false);
    auto alternate=output/"paused-selection.wav";wave(alternate,.5);
    auto alternateUtf8=alternate.u8string();std::memset(packet.path,0,sizeof(packet.path));std::memcpy(packet.path,alternateUtf8.data(),std::min(alternateUtf8.size(),sizeof(packet.path)-1));
    phase("song_path_switch_while_paused",1,0,2,1,1,0,true,0,false,false,false);
    phase("switched_song_resume_at_current_position",1,440,2,3,1,0,true,0,false,false,false);
    std::memset(packet.path,0,sizeof(packet.path));
    phase("temporary_empty_path_while_paused",1,0,2,1,1,0,true,0,false,false,false);
    std::memcpy(packet.path,alternateUtf8.data(),std::min(alternateUtf8.size(),sizeof(packet.path)-1));
    phase("restored_path_remains_paused",1,0,2,1,1,0,true,0,false,false,false);
    phase("restored_path_resumes_at_current_position",1,440,2,3,1,0,true,0,false,false,false);
    std::memset(packet.path,0,sizeof(packet.path));std::memcpy(packet.path,utf8.data(),std::min(utf8.size(),sizeof(packet.path)-1));
    phase("death_stop_silence",1,0,1.2,1);
    phase("epoch_reset_seek",1,880,2,3);
    phase("rate_half",1,220,0,3,.5);
    phase("positive_offset_seek",1,880,0,3,1,2);
    phase("negative_offset_silence",1,0,0,3,1,-2);
    phase("effects_qpc_timestamp",1,-1,0,1,1,0,true,true);
    phase("effects_fmod_44100_burst_continuity",2,-1,0,1,1,0,true,2);
    std::strcpy(packet.status,"Complete");
    phase("music_continues_on_completion_screen",1,880,2,3);
    phase("completed_music_stops_at_file_end",1,0,6.2,3);
    phase("effects_continue_after_level_complete",1,-1,0,1,1,0,true,2);
    std::strcpy(packet.status,"Ready");
    phase("effects_menu_continuity",1,-1,0,1,1,0,true,2);
    std::strcpy(packet.status,"playing");
    phase("combined_music_and_effects",1,440,0,3,1,0,true,true,false,true);
    phase("extreme_offset_music_silent_effects_play",1,-1,0,3,1,1e9,true,true,false,true);
    obs_source_set_muted(source,true);phase("obs_source_mute_flag",1,440,0,3,1,0,true,false,true);obs_source_set_muted(source,false);
    obs_source_dec_active(source);phase("inactive_music_and_effects_silence",1,0,0,3,1,0,true,true);obs_source_inc_active(source);
    phase("link_timeout_silence",1,0,0,3,1,0,false);
    auto delayed=output/"delayed-download.wav";
    std::filesystem::remove(delayed);
    auto delayedUtf8=delayed.u8string();std::memset(packet.path,0,sizeof(packet.path));std::memcpy(packet.path,delayedUtf8.data(),std::min(delayedUtf8.size(),sizeof(packet.path)-1));
    phase("missing_song_silence",1,0,0,3);
    wave(delayed);
    phase("missing_song_same_path_and_epoch_recovery",2,-1,0,3,1,0,true,0,false,false,false,1.3);
    auto replaced=output/"incomplete-download.wav";
    {std::ofstream incomplete(replaced,std::ios::binary);incomplete<<"incomplete download";}
    auto replacedUtf8=replaced.u8string();std::memset(packet.path,0,sizeof(packet.path));std::memcpy(packet.path,replacedUtf8.data(),std::min(replacedUtf8.size(),sizeof(packet.path)-1));
    phase("invalid_song_retry_effects_continuity",2,-1,0,3,1,0,true,2);
    wave(replaced);
    phase("invalid_song_same_path_and_epoch_recovery",2,-1,0,3,1,0,true,0,false,false,false,1.3);
#ifndef _WIN32
    auto growing=output/"growing-download.wav";
    wave(growing);
    std::filesystem::resize_file(growing,44+4800*4);
    auto growingUtf8=growing.u8string();std::memset(packet.path,0,sizeof(packet.path));std::memcpy(packet.path,growingUtf8.data(),std::min(growingUtf8.size(),sizeof(packet.path)-1));
    phase("partial_valid_song_exhausted",1,0,2,3);
    wave(growing);
    phase("growing_song_same_path_and_epoch_recovery",2,880,1,3,1,0,true,0,false,false,false,1.3);
#endif
    if(argc>3){
        std::strncpy(packet.path,argv[3],sizeof(packet.path)-1);phase("mp3_decode",1,-1,0,3);
        phase("failed_seek_music_silence",1,0,100,3);
        phase("failed_seek_effects_continue",1,-1,100,3,1,0,true,true,false,true);
        phase("failed_seek_restart_recovery",1,-1,0,3);
    }
    std::memset(packet.path,0,sizeof(packet.path));std::memcpy(packet.path,utf8.data(),std::min(utf8.size(),sizeof(packet.path)-1));
    // A different session can take ownership only after the prior sender is stale.
    phase("session_handoff_quiet_window",2.2,0,0,0,1,0,false);
    packet.version=5;packet.sessionID=0x51414e4154495645ULL;
    phase("v5_clock_synchronized_playback",1,440,0,3);
    packet.triggerGain=0;phase("v5_trigger_volume_zero",1,0,0,3);
    packet.triggerGain=.5f;packet.musicVolume=1;
    auto gainResult=phase("v5_trigger_and_obs_independent_gain",1,-1,0,3);
    if(gainResult.rms<.09||gainResult.rms>.105)++failures;
    packet.triggerGain=1;packet.musicVolume=1;
    phase("v5_pause_silence",1,0,0,1);
    phase("v5_resume_playback",1,440,0,3);
    phase("v5_effects_share_clock_mapping",1,-1,0,1,1,0,true,1);
    finishClock=true;clockResponder.join();
    json<<"\n],\"failures\":"<<failures<<",\"scope\":\"Native source PCM callbacks; synthetic protocol; not physical audio or live Geometry Dash\"}\n";json.close();
    closeSocket(sock);obs_source_remove_audio_capture_callback(source,Capture::callback,&capture);obs_source_dec_active(source);obs_source_release(source);
    obs_shutdown();return failures?1:0;
}
