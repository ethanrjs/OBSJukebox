#pragma once
#include <cstdint>
#include <limits>
#include <cmath>
#include <algorithm>
#include "../src/LinkPacket.hpp"
class LinkClock {
    int64_t offset=0,targetOffset=0;
    uint64_t adjustmentAt=0;
    uint64_t bestRTT=UINT64_MAX,bestAt=0;
    bool ready=false;
    int64_t offsetAt(uint64_t timestamp)const {
        // Slew in sender-clock time, at 100 ppm (1 us per 10 ms audio packet). Applying a
        // complete correction to only newly received packets would tear the buffered timeline.
        auto elapsed=timestamp>adjustmentAt?timestamp-adjustmentAt:0;
        auto limit=int64_t(elapsed/10000);
        return offset+std::clamp(targetOffset-offset,-limit,limit);
    }
public:
    bool valid()const{return ready;}
    void reset(){ready=false;bestRTT=UINT64_MAX;bestAt=0;}
    bool observe(const ClockSyncPacket& p,uint64_t t4,bool& changed){
        changed=false;
        if(p.t3<p.t2 || t4<p.t1 || t4-p.t1>100000000 || p.t3-p.t2>t4-p.t1 ||
           p.t1>INT64_MAX || p.t2>INT64_MAX || p.t3>INT64_MAX || t4>INT64_MAX)return false;
        auto rtt=(t4-p.t1)-(p.t3-p.t2);
        auto candidate=((int64_t(p.t1)-int64_t(p.t2))/2)+((int64_t(t4)-int64_t(p.t3))/2);
        bool jump=ready && std::abs(double(candidate)-double(offsetAt(p.t3)))>50000000;
        // A probe's error is at most half its round trip, so keep the fastest one. Slower probes
        // win as the best one ages (1 ms of slack per second), which follows clock drift without
        // letting a single delayed reply move the offset. Only a jump (suspend, clock change)
        // invalidates buffered audio.
        if(!ready || jump || rtt<=bestRTT+(t4-bestAt)/1000){
            changed=jump;
            offset=!ready || jump?candidate:offsetAt(p.t3);
            targetOffset=candidate;adjustmentAt=p.t3;
            bestRTT=rtt;bestAt=t4;ready=true;
        }
        return true;
    }
    bool translate(uint64_t& timestamp)const {
        if(!ready || timestamp>INT64_MAX)return false;
        auto correction=offsetAt(timestamp);
        if(correction<0 && timestamp<uint64_t(-correction))return false;
        if(correction>0 && timestamp>uint64_t(INT64_MAX-correction))return false;
        timestamp=uint64_t(int64_t(timestamp)+correction);return true;
    }
};
inline float songGainAt(const SongLinkPacket& p,uint64_t time){
    uint64_t elapsed=time>p.timestamp?time-p.timestamp:0,previous=0;
    float gain=p.triggerGain;
    for(unsigned i=0;i<p.fadeCount;++i){
        const auto& point=p.fades[i];
        if(elapsed<point.offsetNs){
            auto blend=double(elapsed-previous)/double(point.offsetNs-previous);
            return float(gain+(point.gain-gain)*blend);
        }
        previous=point.offsetNs;gain=point.gain;
    }
    return gain;
}
