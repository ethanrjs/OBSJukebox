
static void sourceRecoveryTest(bool failedSeek) {
    receiver=std::make_shared<Receiver>();resetOutput();
    SyncedSender sender(0);
    check(sender.synchronize(),"recovery test synchronizes its sender");
    const char* missing="artifacts/audio-tests/recovered.wav";
    if(!failedSeek)std::filesystem::remove(missing);
    SongLinkPacket packet;packet.flags=3;packet.epoch=91;
    std::strcpy(packet.path,failedSeek?"artifacts/audio-tests/short.mp3":missing);
    auto sendFor=[&](int milliseconds,double position){
        for(int elapsed=0;elapsed<milliseconds;elapsed+=10){
            packet.position=position+elapsed/1000.;sender.send(packet,os_gettime_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        sendFor(failedSeek?250:1000,0);
        if(failedSeek){sendFor(500,100);++packet.epoch;}
        else writeRamp(missing);
        auto recoveredAt=os_gettime_ns();
        sendFor(failedSeek?500:2000,0);
        unsigned blocks=0;double squares=0;unsigned samples=0;
        for(const auto& block:capturedBlocks())if(block.wall>recoveredAt+(failedSeek?100000000:1300000000)){
            ++blocks;for(float sample:block.samples){squares+=sample*sample;++samples;}
        }
        double rms=samples?std::sqrt(squares/samples):0;
        std::cout<<(failedSeek?"Seek":"Missing file")<<" recovery RMS: "<<rms<<", blocks: "<<blocks<<'\n';
        check(blocks>=20 && rms>.005,failedSeek?"MP3 restarts after failed beyond-end seek":"missing file recovers at same path and epoch after file appears");
    }
    receiver.reset();
}

// The frequency of captured blocks whose wall time falls in [from, to).
static double capturedHz(uint64_t from,uint64_t to) {
    unsigned crossings=0,frames=0;float previous=0;bool first=true;
    for(const auto& block:capturedBlocks())if(block.wall>=from&&block.wall<to)
        for(size_t i=0;i<480;++i){
            auto sample=block.samples[i*2];
            if(!first)crossings+=previous<0&&sample>=0;
            previous=sample;first=false;++frames;
        }
    return frames?double(crossings)/(double(frames)/48000.):0.;
}

static void sourceResyncRecoveryTest() {
    receiver=std::make_shared<Receiver>();resetOutput();
    writeTone("artifacts/audio-tests/tone.wav");
    writeTone("artifacts/audio-tests/tone-high.wav",660);
    SyncedSender sender(0);
    check(sender.synchronize(),"output resync test synchronizes its sender");
    SongLinkPacket packet;packet.flags=3;packet.position=.5;
    std::strcpy(packet.path,"artifacts/audio-tests/tone.wav");
    // A group stage is primed before the stall. Its 4096 FFT holds 64 ms of audio, so a chain that
    // wrongly survives the resync would replay the old 220 Hz before the new 330 Hz.
    SongLinkPacket global;global.channelID=-1;global.flags=1;global.stageCount=1;
    global.stages[0]={.5f,4096,0,91};
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        auto sendFor=[&](unsigned count){for(unsigned i=0;i<count;++i){
            sender.send(packet,os_gettime_ns());sender.send(global,os_gettime_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }};
        sendFor(20);
        bool heard=false;for(const auto& block:capturedBlocks())for(float sample:block.samples)heard|=sample>.05f;
        check(heard,"output resync test starts audible music");
        auto resets=source.testMusicResets.load();
        auto delayedAt=os_gettime_ns();source.testOutputDelayMs=180;
        std::strcpy(packet.path,"artifacts/audio-tests/tone-high.wav");packet.position=0;
        sendFor(70);
        unsigned audible=0,blocks=0;
        for(const auto& block:capturedBlocks())if(block.wall>delayedAt+300000000){
            ++blocks;if(std::any_of(block.samples.begin(),block.samples.end(),[](float sample){return sample>.05f;}))++audible;
        }
        const auto hz=capturedHz(delayedAt+300000000,delayedAt+500000000);
        std::cout<<"Resync settled audio: "<<hz<<" Hz\n";
        check(blocks>=20 && audible>=blocks-2,"music resumes after injected output stall resynchronizes timestamp grid");
        check(source.testMusicResets.load()>resets,"the resync resets the stage history");
        check(hz>300 && hz<350,"the resync leaves the new stage processing");
    }
    receiver.reset();
}

static void sourceToggleRecoveryTest() {
    receiver=std::make_shared<Receiver>();resetOutput();
    writeTone("artifacts/audio-tests/tone.wav");
    writeTone("artifacts/audio-tests/tone-high.wav",660);
    SyncedSender sender(0);
    check(sender.synchronize(),"source toggle test synchronizes its sender");
    SongLinkPacket packet;packet.flags=3;packet.position=.5;
    std::strcpy(packet.path,"artifacts/audio-tests/tone.wav");
    SongLinkPacket global;global.channelID=-1;global.flags=1;global.stageCount=1;
    global.stages[0]={.5f,4096,0,92};
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        auto sendFor=[&](unsigned count){for(unsigned i=0;i<count;++i){
            sender.send(packet,os_gettime_ns());sender.send(global,os_gettime_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }};
        sendFor(20);
        bool heard=false;for(const auto& block:capturedBlocks())for(float sample:block.samples)heard|=sample>.05f;
        check(heard,"source toggle test starts audible music");
        source.active=false;
        auto offAt=os_gettime_ns();
        sendFor(20);
        bool silent=true;
        for(const auto& block:capturedBlocks())if(block.wall>offAt+50000000)
            for(float sample:block.samples)silent&=sample==0;
        check(silent,"a deactivated source is silent");
        // Baseline after the deactivation reset, so this only proves the reactivation reset.
        auto resets=source.testMusicResets.load();
        std::strcpy(packet.path,"artifacts/audio-tests/tone-high.wav");packet.position=0;
        auto onAt=os_gettime_ns();
        source.active=true;
        sendFor(50);
        const auto hz=capturedHz(onAt+200000000,onAt+400000000);
        std::cout<<"Reactivation settled audio: "<<hz<<" Hz\n";
        check(source.testMusicResets.load()>resets,"reactivation resets the stage history");
        check(hz>300 && hz<350,"reactivation leaves the new stage processing");
    }
    receiver.reset();
}

static void decoderSeekRecoveryTest() {
    for(double target:{0.,.000006,.02,.5}){
        Decoder decoder;
        bool opened=decoder.open("artifacts/audio-tests/short.mp3");
        bool rejected=true;
        for(unsigned i=0;i<8;++i)rejected&=!decoder.seek(100.+i);
        bool recovered=decoder.seek(target);
        std::vector<float> samples(8192);
        decoder.render(samples.data(),4096,1.);
        double squares=0;for(float sample:samples)squares+=sample*sample;
        double rms=std::sqrt(squares/samples.size());
        std::cout<<"Direct MP3 recovery at "<<target<<" s RMS: "<<rms<<'\n';
        check(opened && rejected && recovered && rms>.01,"decoder resumes near start after repeated beyond-end seeks");
    }
}
// Decoder::open finds an MP3's seek points and length in one pass (indexMp3). Seeks must land on the
// samples miniaudio's own seek points give, and the length must match, with and without a Xing header.
// At rate 1 a fresh seek renders the decoded samples unchanged.
static void decoderSeekPointsTest(){
    const std::pair<const char*,const char*> files[]={
        {"artifacts/audio-tests/long.mp3","MP3 seek points and length match miniaudio's"},
        {"artifacts/audio-tests/long-noxing.mp3","MP3 seek points and length match miniaudio's without a Xing header"}};
    for(const auto& [path,label]:files){
        Decoder decoder;
        auto config=ma_decoder_config_init(ma_format_f32,2,48000);config.seekPointCount=2048;
        ma_decoder reference;
        bool same=decoder.open(path) && ma_decoder_init_file(path,&config,&reference)==MA_SUCCESS;
        if(!same){check(false,label);continue;}
        ma_uint64 length=0;ma_decoder_get_length_in_pcm_frames(&reference,&length);
        same&=decoder.hasAudioAt(double(length-4802)/48000) && !decoder.hasAudioAt(double(length-4799)/48000);
        std::vector<float> ours(4800*2),theirs(4800*2);
        for(double at:{37.3,233.7,.5,120.01}){
            same&=decoder.seek(at);decoder.render(ours.data(),4800,1.);
            same&=ma_decoder_seek_to_pcm_frame(&reference,ma_uint64(at*48000))==MA_SUCCESS;
            same&=ma_decoder_read_pcm_frames(&reference,theirs.data(),4800,nullptr)==MA_SUCCESS;
            same&=ours==theirs;
        }
        ma_decoder_uninit(&reference);
        check(same,label);
    }
}
static void decoderWatchdogTest(bool failedSeek = false) {
    Simulation sim;
    SongLinkPacket packet;packet.flags=3;packet.epoch=310;packet.rate=1.25;packet.offset=.15;
    std::strcpy(packet.path,"artifacts/audio-tests/ramp.wav");
    sim.start();
    auto base=sim.now;
    auto sendFor=[&](unsigned count){for(unsigned i=0;i<count;++i){
        auto at=sim.now;packet.position=.4+double(at-base)/1e9*packet.rate;
        sim.send(packet,at);sim.advance(10000000);
    }};
    sendFor(20);
    auto opens=Decoder::testOpenCount.load();
    if(failedSeek)Decoder::testFailOnNextSeek=true;
    else Decoder::testStarveOnNextSeek=true;
    ++packet.epoch;
    auto faultAt=sim.now;sendFor(90);
    unsigned checked=0;bool aligned=true;uint64_t restored=0;
    for(const auto& block:sim.output)if(block.timestamp>faultAt+50000000){
        if(block.samples.front()>.05f && !restored)restored=block.timestamp;
        if(block.timestamp<faultAt+400000000)continue;
        double expected=.1+(.4+packet.offset+double(block.timestamp-base)/1e9*packet.rate)*.1;
        aligned &= std::abs(block.samples.front()-expected)<.001;
        ++checked;
    }
    check(Decoder::testOpenCount.load()==opens+1,failedSeek?"watchdog reopens after persistent in-range seek failure":"watchdog reopens a decoder that accepts a seek but yields no frames");
    check(restored>faultAt && restored<faultAt+400000000,"stalled music recovers automatically within 400 ms without another pause");
    check(checked>=30 && aligned,"recovered audio follows the current timestamp, playback rate and user offset");
    packet.flags=1;opens=Decoder::testOpenCount.load();sendFor(30);
    check(Decoder::testOpenCount.load()==opens,"watchdog never restarts a paused song");
    Decoder::testStarveOnNextSeek=false;Decoder::testFailOnNextSeek=false;
}

static void decoderWatchdogSilenceTest() {
    const char* path="artifacts/audio-tests/silence.wav";
    writeRamp(path);
    { std::fstream file(path,std::ios::in|std::ios::out|std::ios::binary);file.seekp(44);
      std::vector<char> zeros(48000*4*4);file.write(zeros.data(),zeros.size()); }
    Simulation sim;
    SongLinkPacket packet;packet.flags=3;packet.position=.5;std::strcpy(packet.path,path);
    sim.start();
    auto sendFor=[&](unsigned count){for(unsigned i=0;i<count;++i){
        sim.send(packet,sim.now);sim.advance(10000000);
    }};
    sendFor(20);auto opens=Decoder::testOpenCount.load();sendFor(40);
    check(Decoder::testOpenCount.load()==opens,"valid silent PCM does not trigger decoder recovery");
    packet.position=10;sendFor(40);
    check(Decoder::testOpenCount.load()==opens,"actual song EOF does not trigger decoder recovery");
    packet.position=0;packet.offset=-2;sendFor(30);
    check(Decoder::testOpenCount.load()==opens,"negative offset lead-in stays silent without decoder recovery");
}

static void decoderWatchdogBackoffTest() {
    receiver=std::make_shared<Receiver>();resetOutput();
    SyncedSender sender(0);
    check(sender.synchronize(),"decoder backoff test synchronizes its sender");
    SongLinkPacket packet;packet.flags=3;packet.position=.5;
    std::strcpy(packet.path,"artifacts/audio-tests/ramp.wav");
    Decoder::testStarveAllReads=true;
    auto opens=Decoder::testOpenCount.load();
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        for(unsigned i=0;i<170;++i){
            sender.send(packet,os_gettime_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        auto attempts=Decoder::testOpenCount.load()-opens;
        check(attempts>=2 && attempts<=4,"persistent decoder faults are retried with bounded backoff");
        Decoder::testStarveAllReads=false;
        auto recoverAt=os_gettime_ns();
        for(unsigned i=0;i<40;++i){
            sender.send(packet,os_gettime_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        unsigned heard=0;
        for(const auto& block:capturedBlocks())if(block.timestamp>recoverAt+150000000 && block.samples.front()>.1f)++heard;
        check(heard>=10,"music recovers after a persistent decoder fault clears");
    }
    Decoder::testStarveAllReads=false;receiver.reset();
}
