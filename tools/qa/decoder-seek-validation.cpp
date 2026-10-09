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
        auto utf8=std::filesystem::path(argv[i]).u8string();
        std::string path(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        auto opening=std::chrono::steady_clock::now();
        bool pass=decoder.open(path);
        auto openMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-opening).count();
        decoder.render(audio.data(),480,1);
        pass=pass && audible();
        bool failedSeek=false;
        for(int n=0;n<20;++n) {
            bool sought=decoder.seek(100+n*.01);
            failedSeek|=!sought;
            if(!sought)pass=pass && !decoder.error.empty();
            audio.fill(1.f);
            decoder.render(audio.data(),480,1);
            pass=pass && !audible();
        }
        pass=decoder.seek(0) && pass;
        decoder.render(audio.data(),480,1);
        pass=pass && audible() && decoder.error.empty();

        pass=!decoder.seek(INFINITY) && pass;
        audio.fill(1.f);decoder.render(audio.data(),480,1);
        pass=pass && !audible();
        pass=decoder.seek(0) && pass;
        decoder.render(audio.data(),480,1);
        pass=pass && audible() && decoder.error.empty();

        // A practice-mode respawn seeks backwards mid-song. The decoder thread runs at most two
        // 10 ms blocks ahead of output, so a seek that takes longer than one block risks a gap.
        decoder.seek(60);decoder.render(audio.data(),480,1);
        auto started=std::chrono::steady_clock::now();
        pass=decoder.seek(30) && pass;
        auto seekMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        bool fast=seekMs<10;
        std::printf("%s: %s (backend rejected EOF seek: %s, open %.1f ms, backward seek %.1f ms%s)\n",path.c_str(),pass && fast?"PASS":"FAIL",
            failedSeek?"yes":"no",openMs,seekMs,!pass?", decode checks failed":!fast?", too slow":"");
        pass=pass && fast;
        if(!pass)++failures;
    }
    return failures?1:0;
}
