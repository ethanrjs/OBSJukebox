#include "../../obs-plugin/Decoder.hpp"
#include <fstream>
#include <cstdio>
#include <chrono>

static void fixture(const std::filesystem::path& path){
    std::ofstream out(path,std::ios::binary);uint32_t frames=44100*3,size=frames*2;
    auto put=[&](auto n){out.write(reinterpret_cast<const char*>(&n),sizeof(n));};
    out.write("RIFF",4);put(size+36);out.write("WAVEfmt ",8);put(uint32_t(16));put(uint16_t(1));put(uint16_t(1));
    put(uint32_t(44100));put(uint32_t(88200));put(uint16_t(2));put(uint16_t(16));out.write("data",4);put(size);
    for(uint32_t i=0;i<frames;++i){double hz=i<44100?440:880;put(int16_t(std::sin(6.283185307179586*hz*i/44100)*9000));}
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv){
    if(argc==3 && std::wstring(argv[1])==L"--fixture"){fixture(argv[2]);return 0;}
#else
int main(int argc,char** argv){
    if(argc==3 && std::string(argv[1])=="--fixture"){fixture(argv[2]);return 0;}
#endif
    if(argc<2)return 2;int failures=0;
    for(int i=1;i<argc;++i){
        auto utf8=std::filesystem::path(argv[i]).u8string();std::string path(reinterpret_cast<const char*>(utf8.data()),utf8.size());
        auto started=std::chrono::steady_clock::now();Decoder decoder;bool pass=decoder.open(path);
        std::array<float,960> block{};
        auto signal=[&](double hz,double rate){

            for(unsigned warmup=0;warmup<5;++warmup)decoder.render(block.data(),480,rate);
            double squares=0;size_t crossings=0;float previous=0;
            for(unsigned n=0;n<25;++n){decoder.render(block.data(),480,rate);for(unsigned f=0;f<480;++f){float x=block[f*2];squares+=x*x;if(previous<=0&&x>0)++crossings;previous=x;}}
            double rms=std::sqrt(squares/12000),actual=crossings*48000.0/12000;
            bool valid=hz==0?rms<.00001:rms>.05&&std::abs(actual-hz)<std::max(8.0,hz*.06);
            std::printf(" [rms %.5f hz %.1f expected %.1f %s]",rms,actual,hz,valid?"pass":"FAIL");return valid;
        };
        if(pass){
            pass=signal(440,1)&&pass;
            pass=decoder.seek(1.25)&&pass;pass=signal(880,1)&&pass;
            pass=decoder.seek(0)&&pass;pass=signal(220,.5)&&pass;
            pass=decoder.seek(0)&&pass;pass=signal(880,2)&&pass;

            for(unsigned n=0;n<3;++n){decoder.seek(100);pass=signal(0,1)&&pass;pass=decoder.seek(0)&&pass;pass=signal(440,1)&&pass;}
            pass=!decoder.seek(INFINITY)&&pass;pass=signal(0,1)&&pass;
            pass=decoder.seek(0)&&pass;pass=signal(440,1)&&pass;
        }
        auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
        std::printf("\n%s: %s %.1f ms error=%s\n",path.c_str(),pass?"PASS":"FAIL",ms,decoder.error.c_str());
        if(!pass)++failures;
    }
    return failures?1:0;
}
