
static void sourceRecoveryTest(bool failedSeek) {
    receiver=std::make_shared<Receiver>();resetOutput();
    auto sender=::socket(AF_INET,SOCK_DGRAM,0);
    const char* missing="artifacts/audio-tests/recovered.wav";
    if(!failedSeek)std::filesystem::remove(missing);
    SongLinkPacket packet;packet.flags=3;packet.epoch=91;
    std::strcpy(packet.path,failedSeek?"artifacts/audio-tests/short.mp3":missing);
    auto sendFor=[&](int milliseconds,double position){
        for(int elapsed=0;elapsed<milliseconds;elapsed+=10){
            packet.position=position+elapsed/1000.;sendState(sender,packet,os_gettime_ns());
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
        for(const auto& block:output())if(block.wall>recoveredAt+(failedSeek?100000000:1300000000)){
            ++blocks;for(float sample:block.samples){squares+=sample*sample;++samples;}
        }
        double rms=samples?std::sqrt(squares/samples):0;
        std::cout<<(failedSeek?"Seek":"Missing file")<<" recovery RMS: "<<rms<<", blocks: "<<blocks<<'\n';
        check(blocks>=20 && rms>.005,failedSeek?"MP3 restarts after failed beyond-end seek":"missing file recovers at same path and epoch after file appears");
    }
    closeSocket(sender);receiver.reset();
}

static void sourceResyncRecoveryTest() {
    receiver=std::make_shared<Receiver>();resetOutput();
    auto sender=::socket(AF_INET,SOCK_DGRAM,0);
    SongLinkPacket packet;packet.flags=3;packet.position=.5;
    std::strcpy(packet.path,"artifacts/audio-tests/ramp.wav");
    {
        SongSource source(nullptr);source.active=true;source.worker=std::thread([&]{source.run();});
        auto sendFor=[&](unsigned count){for(unsigned i=0;i<count;++i){
            sendState(sender,packet,os_gettime_ns());std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }};
        sendFor(20);
        bool heard=false;for(const auto& block:output())for(float sample:block.samples)heard|=sample>.05f;
        check(heard,"output resync test starts audible music");
        auto delayedAt=os_gettime_ns();source.testOutputDelayMs=180;
        sendFor(70);
        unsigned audible=0,blocks=0;
        for(const auto& block:output())if(block.wall>delayedAt+300000000){
            ++blocks;if(std::any_of(block.samples.begin(),block.samples.end(),[](float sample){return sample>.05f;}))++audible;
        }
        check(blocks>=20 && audible>=blocks-2,"music resumes after injected output stall resynchronizes timestamp grid");
    }
    closeSocket(sender);receiver.reset();
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
