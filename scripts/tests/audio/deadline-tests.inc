// These stress the real output thread, including a deliberately slow file open.
// Timeline arithmetic is checked by the existing sample tests; wall time here
// detects stalls with scheduler tolerance rather than asserting hard realtime.
static void musicDeadlineTest(bool delayedOpen) {
    receiver=std::make_shared<Receiver>();
    auto sender=socket(AF_INET,SOCK_DGRAM,0);
    resetOutput();
    Decoder::testOpenDelayMs=delayedOpen?180:0;
    SongLinkPacket p;p.flags=3;p.position=190;p.effectsVolume=1;
    std::strcpy(p.path,"artifacts/audio-tests/long.mp3");
    sendState(sender,p,os_gettime_ns());
    {
        SongSource source(nullptr);source.active=true;
        source.worker=std::thread([&]{source.run();});
        sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(receiverPort());addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        for(unsigned i=0;i<90;++i){
            auto now=os_gettime_ns();
            p.position=190+(i%10)*.01;p.epoch=i/10;
            p.flags=i%10==4?1:3;
            if(i>40){p.flags=7;p.loopStart=190;p.loopEnd=190.02;p.position=190;}
            sendState(sender,p,now);
            EffectsPacket effect;effect.frames=480;effect.timestamp=now;
            std::fill_n(effect.samples,960,.02f);
            sendto(sender,reinterpret_cast<const char*>(&effect),36+effect.frames*8,0,reinterpret_cast<sockaddr*>(&addr),sizeof(addr));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    Decoder::testOpenDelayMs=0;
    auto blocks=output();uint64_t maximumGap=0;unsigned earlyEffects=0;bool music=false;
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
    receiver.reset();closeSocket(sender);
}
