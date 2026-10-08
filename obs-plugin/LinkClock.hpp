#pragma once
#include <cstdint>
#include <limits>
#include <cmath>
#include "../src/LinkPacket.hpp"
class LinkClock {
    int64_t offset=0;
    uint64_t bestRTT=UINT64_MAX,bestAt=0;
    bool ready=false;
public:
    bool valid()const{return ready;}
    void reset(){ready=false;bestRTT=UINT64_MAX;bestAt=0;}
    bool observe(const ClockSyncPacket& p,uint64_t t4,bool& changed){
        changed=false;
        if(p.t3<p.t2 || t4<p.t1 || t4-p.t1>100000000 || p.t3-p.t2>t4-p.t1 ||
           p.t1>INT64_MAX || p.t2>INT64_MAX || p.t3>INT64_MAX || t4>INT64_MAX)return false;
        auto rtt=(t4-p.t1)-(p.t3-p.t2);
        auto candidate=((int64_t(p.t1)-int64_t(p.t2))/2)+((int64_t(t4)-int64_t(p.t3))/2);
        bool jump=ready && std::abs(double(candidate)-double(offset))>50000000;
        if(!ready || jump || t4-bestAt>5000000000ULL || rtt<=bestRTT){
            changed=ready && std::abs(double(candidate)-double(offset))>5000000;
            offset=candidate;bestRTT=rtt;bestAt=t4;ready=true;
        }
        return true;
    }
    bool translate(uint64_t& timestamp)const {
        if(!ready || timestamp>INT64_MAX)return false;
        if(offset<0 && timestamp<uint64_t(-offset))return false;
        if(offset>0 && timestamp>uint64_t(INT64_MAX-offset))return false;
        timestamp=uint64_t(int64_t(timestamp)+offset);return true;
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
