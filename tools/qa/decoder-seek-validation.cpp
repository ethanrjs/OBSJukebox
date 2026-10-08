#include "../../obs-plugin/Decoder.hpp"
#include <chrono>
#include <cstdio>

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
#else
int main(int argc,char** argv) {
#endif
    if(argc<2)return 2;
    int failures=0;
    for(int i=1;i<argc;++i) {
        Decoder decoder;
        std::array<float,960> audio{};
        auto audible=[&]{return std::any_of(audio.begin(),audio.end(),[](float x){return std::abs(x)>.00001f;});};
        auto silent=[&]{return std::all_of(audio.begin(),audio.end(),[](float x){return std::isfinite(x) && std::abs(x)<=.00001f;});};
        auto utf8=std::filesystem::path(argv[i]).u8string();
        std::string path(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        // These fixtures use miniaudio formats. Read their actual length instead of assuming
        // 100 seconds is past EOF; Windows-only fallback formats have their own validation tool.
        ma_decoder metadata{};
        auto config=ma_decoder_config_init(ma_format_f32,2,48000);
#ifdef _WIN32
        auto initialized=ma_decoder_init_file_w(argv[i],&config,&metadata);
#else
        auto initialized=ma_decoder_init_file(argv[i],&config,&metadata);
#endif
        ma_uint64 frames=0;
        bool lengthKnown=initialized==MA_SUCCESS && ma_decoder_get_length_in_pcm_frames(&metadata,&frames)==MA_SUCCESS;
        // Vorbis streams report no length up front; count their frames instead.
        if(lengthKnown && !frames){
            std::array<float,4096*2> scratch{};ma_uint64 read=0;
            do{read=0;ma_decoder_read_pcm_frames(&metadata,scratch.data(),4096,&read);frames+=read;}while(read);
        }
        if(initialized==MA_SUCCESS)ma_decoder_uninit(&metadata);
        if(!lengthKnown || frames<3*48000){
            std::printf("%s: FAIL (need a supported fixture at least three seconds long)\n",path.c_str());
            ++failures;continue;
        }
        double duration=double(frames)/48000;
        double target=std::min(30.0,std::floor(duration/3));
        // Check recovered audio against a fresh decoder at the same target, after one block
        // for the resampler to settle. Silence is legitimate; this checks finite output and
        // audio presence, not sample-exact seeking relative to a sequential compressed decode.
        Decoder reference;
        bool referenceOK=reference.open(path);
        std::array<float,960> beginning{},expected{};
        reference.render(beginning.data(),480,1);
        reference.render(beginning.data(),480,1);
        referenceOK=reference.seek(target) && referenceOK;
        reference.render(expected.data(),480,1);
        reference.render(expected.data(),480,1);
        auto matches=[&](const auto& wanted){
            bool expectedAudio=std::any_of(wanted.begin(),wanted.end(),[](float x){return std::abs(x)>.00001f;});
            return std::all_of(audio.begin(),audio.end(),[](float x){return std::isfinite(x);}) && audible()==expectedAudio;
        };
        auto opening=std::chrono::steady_clock::now();
        bool pass=decoder.open(path);
        auto openMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-opening).count();
        decoder.render(audio.data(),480,1);
        decoder.render(audio.data(),480,1);
        pass=pass && referenceOK && matches(beginning);
        bool failedSeek=false;
        for(int n=0;n<20;++n) {
            bool sought=decoder.seek(duration+1+n*.01);
            failedSeek|=!sought;
            if(!sought)pass=pass && !decoder.error.empty();
            audio.fill(1.f);
            decoder.render(audio.data(),480,1);
            pass=pass && silent();
        }
        pass=decoder.seek(0) && pass;
        decoder.render(audio.data(),480,1);
        decoder.render(audio.data(),480,1);
        pass=pass && matches(beginning) && decoder.error.empty();

        pass=!decoder.seek(INFINITY) && pass;
        audio.fill(1.f);decoder.render(audio.data(),480,1);
        pass=pass && silent();
        pass=decoder.seek(0) && pass;
        decoder.render(audio.data(),480,1);
        decoder.render(audio.data(),480,1);
        pass=pass && matches(beginning) && decoder.error.empty();

        // A practice-mode respawn seeks backwards mid-song. The decoder thread runs at most two
        // 10 ms blocks ahead of output, so a seek that takes longer than one block risks a gap.
        // Measure an in-range backward seek plus its first render, including deferred decode work.
        pass=decoder.seek(target*2) && pass;decoder.render(audio.data(),480,1);
        auto started=std::chrono::steady_clock::now();
        pass=decoder.seek(target) && pass;
        decoder.render(audio.data(),480,1);
        auto seekMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        decoder.render(audio.data(),480,1); // compare after the resampler's short seek transient
        pass=pass && matches(expected) && decoder.error.empty();
        bool fast=seekMs<10;
        std::printf("%s: %s (backend rejected EOF seek: %s, open %.1f ms, seek %.0f->%.0f s + render %.1f ms%s)\n",path.c_str(),pass && fast?"PASS":"FAIL",
            failedSeek?"yes":"no",openMs,target*2,target,seekMs,!pass?", decode checks failed":!fast?", too slow":"");
        pass=pass && fast;
        if(!pass)++failures;
    }
    return failures?1:0;
}
