#pragma once
#ifdef _WIN32
#include "WindowsMediaDecoder.hpp"
#endif
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../vendor/miniaudio/miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "../vendor/miniaudio/stb_vorbis.c"
#include <array>
#include <algorithm>
#include <string>
#include <cmath>
#include <cstring>
#include <filesystem>
#ifdef _WIN32
#include <memory>
#endif
class Decoder {
    ma_decoder decoder{};
    bool opened=false, seekValid=false;
#ifdef _WIN32
    std::unique_ptr<WindowsMediaDecoder> windowsDecoder;
#endif
    std::array<float,16384> cache{};
    size_t count=0;
    double phase=0;
public:
    double position=0;
    std::string error;
    ~Decoder(){close();}
    void close(){
#ifdef _WIN32
        if(windowsDecoder)windowsDecoder.reset();else
#endif
        if(opened)ma_decoder_uninit(&decoder);
        opened=false;seekValid=false;count=0;phase=0;position=0;
    }
    bool open(const std::string& path){
        close();error.clear();
        if(path.empty()){error="Choose an OBS song in Jukebox.";return false;}
        auto config=ma_decoder_config_init(ma_format_f32,2,48000);
#ifdef _WIN32
        auto wide=std::filesystem::u8path(path).wstring();
        auto result=ma_decoder_init_file_w(wide.c_str(),&config,&decoder);
#else
        auto result=ma_decoder_init_file(path.c_str(),&config,&decoder);
#endif
        opened=result==MA_SUCCESS;
#ifdef _WIN32
        if(!opened){
            auto fallback=std::make_unique<WindowsMediaDecoder>();
            if(fallback->open(wide)){windowsDecoder=std::move(fallback);opened=true;}
        }
#endif
        seekValid=opened;
        if(!opened){
#ifdef _WIN32
            error="Cannot decode this song. Use MP3, WAV, FLAC, Ogg Vorbis, AAC, M4A or uncompressed AIFF.";
#else
            error="Cannot decode this song. Use MP3, WAV, FLAC, Ogg Vorbis or uncompressed AIFF.";
#endif
        }
        return opened;
    }
    bool ready()const{return opened;}
    bool seek(double seconds){
        count=0;phase=0;seekValid=false;
        if(!opened)return false;
        bool valid=std::isfinite(seconds) && seconds>=0 && seconds<=31536000.0;
        if(valid){
#ifdef _WIN32
            if(windowsDecoder)valid=windowsDecoder->seek(seconds);else
#endif
            valid=ma_decoder_seek_to_pcm_frame(&decoder,ma_uint64(seconds*48000))==MA_SUCCESS;
        }
        if(!valid){
            error="Cannot seek this song. Music is silent until a valid position is available.";return false;
        }
        position=seconds;seekValid=true;error.clear();return true;
    }
    void render(float* output,size_t frames,double rate){
        if(!opened || !seekValid){std::fill_n(output,frames*2,0.f);return;}
        size_t consumed=std::min(count,size_t(phase));
        if(consumed){std::memmove(cache.data(),cache.data()+consumed*2,(count-consumed)*2*sizeof(float));count-=consumed;phase-=consumed;}
        size_t needed=size_t(std::ceil(phase+frames*rate))+2;
        if(count<needed){
            ma_uint64 got=0;
#ifdef _WIN32
            if(windowsDecoder)got=windowsDecoder->read(cache.data()+count*2,cache.size()/2-count);else
#endif
            ma_decoder_read_pcm_frames(&decoder,cache.data()+count*2,cache.size()/2-count,&got);
            count+=got;
        }
        for(size_t i=0;i<frames;++i){
            auto at=size_t(phase);double blend=phase-at;
            for(size_t ch=0;ch<2;++ch){float a=at<count?cache[at*2+ch]:0,b=at+1<count?cache[(at+1)*2+ch]:a;output[i*2+ch]=float(a+(b-a)*blend);}
            phase+=rate;
        }
        position+=frames*rate/48000.;
    }
};
