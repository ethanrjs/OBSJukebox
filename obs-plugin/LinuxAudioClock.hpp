#pragma once
#include <cstdint>
#include <cstdlib>

class LinuxAudioClock {
    int64_t offset=0;
    uint64_t lastReceived=0;
    bool initialized=false;
public:
    bool translate(uint64_t& timestamp,uint64_t now) {
        auto measured=int64_t(now)-int64_t(timestamp);
        bool reset=!initialized || now-lastReceived>1000000000 || std::abs(measured-offset)>1000000000;
        if(reset){offset=measured;initialized=true;}
        lastReceived=now;
        timestamp=uint64_t(int64_t(timestamp)+offset);
        return reset;
    }
};
