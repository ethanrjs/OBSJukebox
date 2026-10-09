#pragma once
#ifdef _WIN32
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class WindowsMediaDecoder {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<IMFSourceReader> reader;
    bool comStarted=false,mfStarted=false,eof=false,trimSeek=false;
    int64_t seekTime=0;
    uint64_t duration=0;
    std::vector<float> pending;
    size_t cursor=0;
    bool validType(){
        Ptr<IMFMediaType> type;GUID subtype{};UINT32 channels=0,rate=0,align=0;
        return SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,&type)) &&
            SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE,&subtype)) && subtype==MFAudioFormat_Float &&
            SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS,&channels)) && channels==2 &&
            SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,&rate)) && rate==48000 &&
            SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,&align)) && align==8;
    }
    bool nextSample(){
        pending.clear();cursor=0;
        for(unsigned attempt=0;attempt<16 && !eof;++attempt){
            Ptr<IMFSample> sample;DWORD flags=0;LONGLONG timestamp=0;
            auto result=reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,nullptr,&flags,&timestamp,&sample);
            if(FAILED(result) || (flags&MF_SOURCE_READERF_ERROR)){eof=true;return false;}
            if((flags&MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !validType()){eof=true;return false;}
            if(flags&MF_SOURCE_READERF_ENDOFSTREAM)eof=true;
            if(!sample)continue;
            Ptr<IMFMediaBuffer> buffer;
            if(FAILED(sample->ConvertToContiguousBuffer(&buffer))){eof=true;return false;}
            BYTE* bytes=nullptr;DWORD length=0;
            if(FAILED(buffer->Lock(&bytes,nullptr,&length))){eof=true;return false;}
            if(length%8 || length>1024*1024){buffer->Unlock();eof=true;return false;}
            size_t frames=length/8,skip=0;
            if(trimSeek && timestamp<seekTime){
                auto missing=std::ceil((double(seekTime)-double(timestamp))*48000.0/10000000.0);
                skip=static_cast<size_t>(std::min(double(frames),std::max(0.0,missing)));
            }
            if(skip<frames){
                pending.resize((frames-skip)*2);
                std::memcpy(pending.data(),bytes+skip*8,pending.size()*sizeof(float));
                trimSeek=false;
            }
            buffer->Unlock();
            if(!pending.empty())return true;
        }
        return false;
    }
public:
    uint64_t lengthInFrames() const { return duration / 10000000 * 48000 + duration % 10000000 * 48000 / 10000000; }
    ~WindowsMediaDecoder(){close();}
    void close(){
        reader.Reset();pending.clear();cursor=0;eof=false;duration=0;trimSeek=false;
        if(mfStarted){MFShutdown();mfStarted=false;}
        if(comStarted){CoUninitialize();comStarted=false;}
    }
    bool open(const std::wstring& path){
        close();
        auto result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(result) && result!=RPC_E_CHANGED_MODE)return false;
        comStarted=SUCCEEDED(result);
        result=MFStartup(MF_VERSION,MFSTARTUP_LITE);
        if(FAILED(result)){close();return false;}mfStarted=true;
        Ptr<IMFMediaType> type;
        if(FAILED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader)) ||
            FAILED(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS,FALSE)) ||
            FAILED(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE)) ||
            FAILED(MFCreateMediaType(&type)) ||
            FAILED(type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Audio)) ||
            FAILED(type->SetGUID(MF_MT_SUBTYPE,MFAudioFormat_Float)) ||
            FAILED(type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,2)) ||
            FAILED(type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,48000)) ||
            FAILED(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,32)) ||
            FAILED(type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,8)) ||
            FAILED(type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,384000)) ||
            FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,nullptr,type.Get())) || !validType()){
            close();return false;
        }
        PROPVARIANT value{};
        if(SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,MF_PD_DURATION,&value)) && value.vt==VT_UI8)
            duration=value.uhVal.QuadPart;
        PropVariantClear(&value);
        return true;
    }
    bool seek(double seconds){
        pending.clear();cursor=0;eof=false;trimSeek=true;
        seekTime=static_cast<int64_t>(seconds*10000000.0);
        if(duration && uint64_t(seekTime)>=duration){eof=true;return true;}
        PROPVARIANT value{};value.vt=VT_I8;value.hVal.QuadPart=seekTime;
        if(FAILED(reader->SetCurrentPosition(GUID_NULL,value))){eof=true;return false;}
        return true;
    }
    size_t read(float* output,size_t frames){
        size_t copied=0;
        while(copied<frames){
            if(cursor>=pending.size() && !nextSample())break;
            size_t available=(pending.size()-cursor)/2,take=std::min(available,frames-copied);
            std::memcpy(output+copied*2,pending.data()+cursor,take*2*sizeof(float));
            cursor+=take*2;copied+=take;
        }
        return copied;
    }
};
#endif
