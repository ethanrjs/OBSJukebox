// Included after Simulation, SyncedSender and the shared test helpers.
static void receiverIsolationTest() {
    Simulation sim;
    auto first=sim.game(0,50001),second=sim.game(0,50003);
    check(sim.synchronize(first),"session isolation establishes first sender");
    SongLinkPacket packet;packet.epoch=811;
    for(int i=0;i<5;++i){sim.send(second,packet,sim.now);sim.advance(10000000);}
    check(receiver->get().packet.sessionID==first.session,"fresh session rejects a competing game sender");
    packet.sessionID=first.session;
    sim.sendRaw(packet,first.stamp(sim.now),7,50005,&first.key);
    sim.advance(20000000);
    check(receiver->get().packet.epoch!=811,"same session from another song port is rejected");
    sim.advance(2050000000);
    check(sim.synchronize(second,812),"stale session permits a replacement sender");
    sim.send(first,packet,sim.now);sim.advance(20000000);
    check(receiver->get().packet.sessionID==second.session,"old sender cannot steal the replacement session");
}
static void receiverEffectsBoundaryTest() {
    Simulation sim;
    auto game=sim.game();
    check(sim.synchronize(game),"effects boundary establishes clock sync");
    SongLinkPacket song;song.epoch=821;
    auto native=sim.now;sim.send(game,song,native);
    sim.advance(15000000);
    auto mapped=receiver->get().packet.timestamp;
    EffectsPacketV3 a;a.sessionID=game.session;a.timestamp=game.stamp(native);a.sampleRate=44100;a.frames=2;a.sequence=10;
    a.samples[0]=a.samples[1]=0;a.samples[2]=a.samples[3]=1;
    auto b=a;b.timestamp=a.timestamp+2*1000000000ULL/44100;b.sequence=11;b.samples[0]=b.samples[1]=2;b.samples[2]=b.samples[3]=3;
    sim.sendEffects(a,game.effectsPort);sim.sendEffects(b,game.effectsPort);
    sim.advance(15000000);
    float audio[2]{};receiver->mixEffects(audio,1,mapped+34013,1);
    check(std::abs(audio[0]-1.5f)<.001f && audio[0]==audio[1],"44.1 kHz interpolation crosses contiguous packet boundaries");
    auto bad=a;bad.timestamp=a.timestamp+10000000;bad.samples[0]=bad.samples[1]=20;
    sim.sendEffects(bad,50009);
    sim.advance(15000000);
    audio[0]=audio[1]=0;receiver->mixEffects(audio,1,mapped+10000000,1);
    check(audio[0]==0,"effects stream rejects another port claiming the active session");
    bad.sessionID=0;sim.sendEffects(bad,game.effectsPort);
    sim.advance(15000000);
    audio[0]=audio[1]=0;receiver->mixEffects(audio,1,mapped+10000000,1);
    check(audio[0]==0,"effects stream rejects a zero session");
}

static void receiverFloodTest() {
    receiver=std::make_shared<Receiver>();
    SyncedSender sender(0);
    check(sender.synchronize(),"packet load test establishes clock sync");
    const auto base=os_gettime_ns()+10000000;
    SongLinkPacket packet;
    for(int channel=0;channel<64;++channel){packet.channelID=channel;sender.send(packet,base-1000000);}
    packet.channelID=64;packet.epoch=900;sender.send(packet,base);
    packet.channelID=0;
    std::vector<unsigned> expected;
    for(unsigned index=0;index<400;++index)if(index%7)expected.push_back(index);
    for(unsigned n=0;n<400;++n){
        const unsigned index=(n*137)%400; // Fixed permutation, independent of wall time.
        if(index%7){packet.epoch=index;sender.send(packet,base+index*100000);}
        if(n%32==31)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    packet.channelID=63;packet.epoch=901;sender.send(packet,base+50000000);
    bool drained=false;
    for(unsigned attempt=0;attempt<100 && !drained;++attempt){
        auto window=receiver->playbackWindow(base+70000000,base+70000001,63);
        drained=window.front().epoch==901;
        if(!drained)std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto channels=receiver->getChannels();
    check(drained && channels.size()==64 && channels.front()==0 && channels.back()==63,"64 voice channels accepted and channel 64 rejected under packet load");
    auto window=receiver->playbackWindow(base-5000000,base+70000000,0);
    if(expected.size()>256)expected.erase(expected.begin(),expected.end()-256);
    bool exact=window.size()==257;
    for(size_t i=1;exact && i<window.size();++i)
        exact=window[i].epoch==expected[i-1] && window[i].timestamp-window[1].timestamp==(expected[i-1]-expected[0])*100000;
    check(exact,"fixed reordered and dropped packet sequence preserves sorted newest 256 states");
    receiver.reset();
}

static void receiverLifetimeTest() {
    receiver=std::make_shared<Receiver>();
    std::weak_ptr<Receiver> lifetime=receiver;
    SyncedSender sender(0);
    check(sender.synchronize(),"receiver lifetime test synchronizes its sender");
    resetOutput();
    SongLinkPacket packet;packet.flags=3;packet.position=.5;
    std::strcpy(packet.path,"artifacts/audio-tests/ramp.wav");
    sender.send(packet,os_gettime_ns());
    const auto handles=Decoder::testOpenHandles.load();
    {
        SongSource source(nullptr);source.active=true;
        source.worker=std::thread([&]{source.run();});
        bool opened=false;
        for(unsigned attempt=0;attempt<50 && !opened;++attempt){
            opened=Decoder::testOpenHandles.load()>handles;
            if(!opened)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(opened,"receiver lifetime test opens a decoder");
        receiver.reset();
        check(!lifetime.expired(),"live source retains receiver after module reference is released");
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        check(Decoder::testOpenHandles.load()==handles,"stale voice releases its decoder file handle while source remains alive");
        auto blocks=capturedBlocks();
        check(blocks.size()>40 && std::all_of(blocks.back().samples.begin(),blocks.back().samples.end(),[](float sample){return sample==0;}),
            "source continues safely and becomes silent after receiver owner reset");
    }
    check(lifetime.expired(),"last source releases retained receiver on destruction");
}
