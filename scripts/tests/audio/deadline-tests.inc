// These stress the real output thread, including a deliberately slow file open.
// Timeline arithmetic is checked by the existing sample tests; wall time here
// detects stalls with scheduler tolerance rather than asserting hard realtime.
struct StressRun {
    double rate=1;
    bool loop=false,checkBudget=true;
    unsigned voices=5,seconds=11,effectsStages=8;
    bool voiceStages=true;
    // Windows' default 15.6 ms timer, which it may give OBS while minimized despite libobs's request.
    bool coarseTimer=false;
    uint64_t effectsLatenessNs=0; // how long after its timestamp each effects packet is sent
    // GD's tap sends 512 frames at the game's output rate, often 44.1 kHz.
    unsigned effectsFrames=480,effectsRate=48000;
};
// A real-worker stress run: five voices carry ten 4096 stages, the global adds a group stage, and
// eight post-tap effects stages share the same hop grid. Measured after a 1 s warm-up; the raw miss
// counts include it. Returns the staged effects frames whose packets came after the worker rendered.
static uint64_t pitchStressRun(const StressRun& run) {
    const auto rate=run.rate;const bool loop=run.loop;
    const unsigned voiceCount=run.voices,effectsStages=run.effectsStages;
#ifdef _WIN32
    if(run.coarseTimer)timeEndPeriod(1);
#endif
    receiver=std::make_shared<Receiver>();resetOutput();
    SyncedSender sender(0);auto effects=socket(AF_INET,SOCK_DGRAM,0);
    check(sender.synchronize(),"pitch stress test synchronizes its sender");
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    SongLinkPacket global;global.channelID=-1;global.flags=1;global.stageCount=1;
    global.stages[0]={.9f,4096,0,900};
    global.effectsStageCount=uint8_t(effectsStages);
    for(unsigned i=0;i<effectsStages;++i)global.effectsStages[i]={.8f+float(i)*.02f,4096,0,910+i};
    std::vector<SongLinkPacket> voices(voiceCount);
    for(unsigned v=0;v<voiceCount;++v){
        auto& p=voices[v];p.flags=3;p.channelID=int(v);p.rate=float(rate);p.epoch=1;
        std::strcpy(p.path,"artifacts/audio-tests/long.mp3");
        if(loop){p.flags|=4;p.loopStart=1;p.loopEnd=3;}
        if(run.voiceStages){
            p.stageCount=v<4?1:6;
            for(unsigned s=0;s<p.stageCount;++s)
                p.stages[s]={.85f+float(v)*.02f+float(s)*.01f,4096,0,920+v*8+s};
        }
    }
    uint64_t media=0,pauseAt=0,lateFrames=0;
    bool paused=false,resetEffects=false;
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        const auto start=os_gettime_ns();auto last=start;
        std::atomic<bool> measuring{false};unsigned rawBefore=0,effectsRawBefore=0;
        // Effects as the tap sends them: back-to-back packets at their own cadence, each sent
        // `effectsLatenessNs` after its timestamp. The sender spins, so a coarse timer adds no lateness,
        // and it runs at audio scheduling, as FMOD's mixer thread does in GD. Its own lateness is
        // taken once each packet is out, so a slow harness cannot pass for a plugin miss.
        std::atomic<bool> stopEffects{false},sendFailed{false};
        std::atomic<uint64_t> sendLateNs{0};
        std::thread effectsSender([&]{
            const AudioThreadScheduling scheduling;
            uint64_t sent=0;unsigned sequence=0;
            while(!stopEffects){
                const auto stamp=start+separate_song::framesToNs(sent,run.effectsRate);
                EffectsPacketV2 effect;effect.sessionID=sender.session;effect.frames=run.effectsFrames;
                effect.sampleRate=run.effectsRate;effect.sequence=sequence++;effect.timestamp=stamp;
                for(unsigned i=0;i<run.effectsFrames;++i)
                    effect.samples[i*2]=effect.samples[i*2+1]=
                        float(.2*std::sin(2*3.14159265358979*440*double(sent+i)/run.effectsRate));
                while(!stopEffects && os_gettime_ns()<stamp+run.effectsLatenessNs)std::this_thread::yield();
                if(stopEffects)break;
                const int size=int(44+effect.frames*8);
                if(sendto(effects,reinterpret_cast<const char*>(&effect),size,0,
                          reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=size)sendFailed=true;
                if(measuring)sendLateNs=std::max<uint64_t>(sendLateNs,os_gettime_ns()-stamp-run.effectsLatenessNs);
                sent+=run.effectsFrames;
            }
        });
        for(;;){
            const auto now=os_gettime_ns();const auto elapsed=now-start;
            if(elapsed>=uint64_t(run.seconds)*1000000000ULL)break;
            if(!measuring&&elapsed>=1000000000){
                source.testMaxMixNs=0;source.testWakeMaxNs=0;source.testMusicMisses=0;source.testEffectsMisses=0;
                source.testLateFrames=0;
                rawBefore=source.testMusicMissesRaw.load();effectsRawBefore=source.testEffectsMissesRaw.load();
                source.testMeasure=true;measuring=true;
            }
            const auto delta=now-last;last=now;
            if(!paused)media+=uint64_t(double(delta)*rate);
            if(elapsed>=6000000000ULL&&elapsed<6100000000ULL&&!paused){paused=true;pauseAt=elapsed;}
            if(paused&&elapsed>=pauseAt+100000000ULL){
                paused=false;for(auto&p:voices)++p.epoch;
            }
            if(!resetEffects&&elapsed>=5000000000ULL&&effectsStages){
                for(unsigned i=0;i<effectsStages;++i)global.effectsStages[i].instance=950+i;
                resetEffects=true;
            }
            double position=1+double(media)/1e9;
            if(loop)position=1+std::fmod(position-1,2.0);
            sender.send(global,now);
            for(auto&p:voices){p.position=position;p.flags=paused?1:(3|(loop?4:0));sender.send(p,now);}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stopEffects=true;effectsSender.join();
        source.testMeasure=false;
        const auto maxMix=source.testMaxMixNs.load(),wake=source.testWakeMaxNs.load();
        const auto misses=source.testMusicMisses.load();
        const auto raw=source.testMusicMissesRaw.load()-rawBefore;
        const auto effectsMisses=source.testEffectsMisses.load();
        const auto effectsRaw=source.testEffectsMissesRaw.load()-effectsRawBefore;
        lateFrames=source.testLateFrames.load();
        std::cout<<"Pitch stress "<<rate<<"x loop="<<loop<<" voices="<<voiceCount<<" effects="<<effectsStages
                 <<(run.coarseTimer?" coarse timer":"")<<" lateness "<<double(run.effectsLatenessNs)/1e6
                 <<" ms: max mix "<<double(maxMix)/1e6<<" ms, misses "<<misses<<"/"<<raw
                 <<", effects misses "<<effectsMisses<<"/"<<effectsRaw<<", late frames "<<lateFrames
                 <<", wake "<<double(wake)/1e6<<" ms, sender late "<<double(sendLateNs.load())/1e6<<" ms\n";
        check(!sendFailed,"the effects sender sends every packet");
        if(run.checkBudget){
            check(sendLateNs<2000000,"the effects sender keeps its cadence");
            check(misses==0,"pitch stress has no missed music blocks");
            check(effectsMisses==0,"pitch stress has no missed effects blocks");
            check(maxMix<=2000000,"pitch stress mixBlock stays within 2 ms");
        }
    }
    closeSocket(effects);receiver.reset();
#ifdef _WIN32
    if(run.coarseTimer)timeBeginPeriod(1);
#endif
    return lateFrames;
}
static void pitchStressTest() {
    check(pitchStressRun({.loop=true})==0,"on-time effects all reach the staged worker in time");
    pitchStressRun({.rate=.01});
    pitchStressRun({.rate=16});
    pitchStressRun({.loop=true,.coarseTimer=true});
    pitchStressRun({.checkBudget=false,.voices=64,.seconds=3,.voiceStages=false});
}
// The worker renders a staged block when the output starts the block before it, at least 20 ms
// after the block's timestamp, so each frame may arrive up to 10 ms after its own time: a packet up
// to 10 ms after its timestamp. A packet 20 ms late has frames 10 to 20 ms late, which are lost.
static void effectsLatenessTest() {
    for(const unsigned rate:{48000u,44100u}){
        StressRun light{.voices=0,.seconds=4,.effectsStages=1};
        if(rate==44100){light.effectsFrames=512;light.effectsRate=44100;}
        auto within=light;within.effectsLatenessNs=5000000;
        auto beyond=light;beyond.effectsLatenessNs=20000000;
        check(pitchStressRun(within)==0,rate==44100?"tap-sized 44.1 kHz staged effects sent 5 ms late lose no frames"
                                                     :"staged effects sent 5 ms late lose no frames");
        check(pitchStressRun(beyond)>0,rate==44100?"tap-sized 44.1 kHz staged effects sent 20 ms late lose frames"
                                                     :"staged effects sent 20 ms late lose frames");
    }
}
// Slow effects work (a burst of 4096 stages, a reset, a preempted thread) must not hold up the output
// or the music. Once it catches up, effects play at their own timestamps: each packet carries a
// marker that names when it was sent, so replayed old audio cannot pass for new. Without SFX stages
// the output mixes effects itself, so they keep playing through the delay.
static void effectsDelayTest(bool staged) {
    receiver=std::make_shared<Receiver>();
    SyncedSender sender(0);auto socketFd=socket(AF_INET,SOCK_DGRAM,0);
    check(sender.synchronize(),"effects delay test synchronizes its sender");
    resetOutput();
    writeFloatWav("artifacts/audio-tests/half.wav",.5f);
    SongLinkPacket music;music.flags=7;music.loopStart=.1;music.loopEnd=.9;music.position=.1;
    std::strcpy(music.path,"artifacts/audio-tests/half.wav");
    SongLinkPacket global;global.channelID=-1;global.flags=1;global.effectsVolume=1;
    // Staged effects are one steady level. A fresh FFT 4096 stage fades it in over its first hops;
    // one that kept its history would play its held audio from before the delay at full level.
    constexpr uint64_t delayNs=180000000;
    if(staged){global.effectsStageCount=1;global.effectsStages[0]={1.f,4096,0,77};}
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    constexpr unsigned markers=40;
    auto marker=[&](unsigned k){return staged?.1f:.004f*float(k%markers+1);};
    std::vector<uint64_t> sentAt;
    uint64_t delayFrom=0;
    {
        SongSource source(nullptr);source.active=true;
        source.worker=std::thread([&]{source.run();});
        for(unsigned k=0;k<150;++k){
            const auto now=os_gettime_ns();
            if(k==60){delayFrom=now;source.testEffectsDelayMs=int(delayNs/1000000);}
            music.position=.1+std::fmod(double(k)*.01,.8);
            sender.send(global,now);sender.send(music,now);
            EffectsPacketV2 effect;effect.sessionID=sender.session;effect.frames=480;effect.sampleRate=48000;
            effect.sequence=k;effect.timestamp=now;std::fill_n(effect.samples,960,marker(k));
            sendto(socketFd,reinterpret_cast<const char*>(&effect),44+effect.frames*8,0,
                   reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
            sentAt.push_back(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    auto blocks=capturedBlocks();
    uint64_t maximumGap=0;
    for(size_t i=1;i<blocks.size();++i)maximumGap=std::max(maximumGap,blocks[i].wall-blocks[i-1].wall);
    auto peak=[](const Captured& block){
        float most=0;for(float s:block.samples)most=std::max(most,std::abs(s-.5f));
        return most;
    };
    auto hasEffects=[&](const Captured& block){return peak(block)>.001f;};
    // During the delay the music keeps playing; staged effects it misses are silence, not a stall.
    unsigned delayedBlocks=0,delayedMusic=0,delayedEffects=0,recovered=0,misplaced=0;
    bool silent=false,silentRunOver=false;
    float onset=0,steadyPeak=0;
    for(const auto& block:blocks){
        if(block.wall<delayFrom)continue;
        const bool delayed=block.wall>delayFrom+20000000 && block.wall<delayFrom+160000000;
        if(delayed){
            ++delayedBlocks;delayedMusic+=std::abs(block.samples.front()-.5f)<.25f;
            delayedEffects+=hasEffects(block);
        }
        // The run of effects-silent blocks that starts with the delay.
        if(!silentRunOver){
            if(!hasEffects(block))silent=true;
            else if(silent){silentRunOver=true;onset=peak(block);}
        }
        if(silentRunOver)steadyPeak=std::max(steadyPeak,peak(block));
        if(block.wall<delayFrom+160000000)continue;
        recovered+=hasEffects(block);
        if(staged)continue;
        // Unstaged markers arrive as sent, so each one must belong to this block's time.
        for(size_t i=0;i<block.samples.size();i+=2){
            const float slot=(block.samples[i]-.5f)/.004f-1;
            if(slot<-.5f || std::abs(slot-std::round(slot))>.05f)continue;
            uint64_t nearest=UINT64_MAX;
            for(unsigned k=unsigned(std::lround(slot))%markers;k<sentAt.size();k+=markers)
                nearest=std::min<uint64_t>(nearest,uint64_t(std::abs(double(int64_t(sentAt[k]-block.timestamp)))));
            if(nearest>25000000){++misplaced;break;}
        }
    }
    std::cout<<"Effects delay staged="<<staged<<": gap "<<double(maximumGap)/1e6<<" ms, music "<<delayedMusic
             <<"/"<<delayedBlocks<<", effects "<<delayedEffects<<", onset "<<onset<<" of "<<steadyPeak
             <<", recovered "<<recovered<<", misplaced "<<misplaced<<'\n';
    check(blocks.size()>=120 && maximumGap<50000000,"a delayed effects render cannot stall output deadlines");
    check(delayedBlocks>=8 && delayedMusic>=delayedBlocks-1,"music keeps playing while effects are delayed");
    check(recovered>=40,"effects recover audibly after the delay");
    if(staged){
        check(delayedEffects==0,"staged effects the worker has not rendered play as silence");
        check(silentRunOver && onset<.5f*steadyPeak,
              "the effects stage restarts after the delay instead of replaying held audio");
    }else{
        check(delayedEffects>=delayedBlocks-1,"effects without stages keep playing while the worker is delayed");
        check(misplaced==0,"recovered effects play at their own timestamps");
    }
    receiver.reset();closeSocket(socketFd);
}
static void musicDeadlineTest(bool delayedOpen) {
    receiver=std::make_shared<Receiver>();
    SyncedSender sender(0);auto effects=socket(AF_INET,SOCK_DGRAM,0);
    check(sender.synchronize(),"deadline test synchronizes its sender");
    resetOutput();
    Decoder::testOpenDelayMs=delayedOpen?180:0;
    SongLinkPacket p;p.flags=3;p.position=190;p.effectsVolume=1;
    std::strcpy(p.path,"artifacts/audio-tests/long.mp3");
    // The global packet carries the game's effects state; the mod sends it every sample.
    SongLinkPacket global;global.channelID=-1;global.flags=1;global.effectsVolume=1;
    sender.send(global,os_gettime_ns());
    sender.send(p,os_gettime_ns());
    {
        SongSource source(nullptr);source.active=true;
        source.worker=std::thread([&]{source.run();});
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        for(unsigned i=0;i<90;++i){
            auto now=os_gettime_ns();
            p.position=190+(i%10)*.01;p.epoch=i/10;
            p.flags=i%10==4?1:3;
            if(i>40){p.flags=7;p.loopStart=190;p.loopEnd=190.02;p.position=190;}
            sender.send(p,now);
            EffectsPacketV2 effect;effect.sessionID=sender.session;effect.frames=480;effect.timestamp=now;
            std::fill_n(effect.samples,960,.02f);
            sendto(effects,reinterpret_cast<const char*>(&effect),44+effect.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    Decoder::testOpenDelayMs=0;
    auto blocks=capturedBlocks();uint64_t maximumGap=0;unsigned earlyEffects=0;bool music=false;
    for(size_t i=1;i<blocks.size();++i)maximumGap=std::max(maximumGap,blocks[i].wall-blocks[i-1].wall);
    for(const auto& block:blocks){
        for(float sample:block.samples)music|=std::abs(sample)>.04f;
        if(!blocks.empty() && block.wall<blocks.front().wall+150000000)
            for(float sample:block.samples)if(sample>.015f){++earlyEffects;break;}
    }
    std::cout<<"Output maximum block gap: "<<double(maximumGap)/1e6<<" ms\n";
    check(blocks.size()>=75 && maximumGap<50000000,
        delayedOpen?"180 ms decoder open cannot stall output deadlines":"late MP3 seeks and 20 ms loops preserve output deadlines");
    check(music,"long MP3 decodes after late seeks and loop wraps");
    if(delayedOpen)check(earlyEffects>=6,"SFX continues during deliberately blocked decoder open");
    receiver.reset();closeSocket(effects);
}
