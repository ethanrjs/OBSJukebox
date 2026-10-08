#include "Bridge.hpp"
#include "MonotonicClock.hpp"
#include <cstring>
#include <algorithm>
namespace separate_song {
namespace {
// Copies at most size-1 bytes without splitting a UTF-8 sequence; the rest of out stays zeroed.
template <size_t N> void copyText(char (&out)[N], const std::string& text) {
    size_t length = std::min(text.size(), N - 1);
    if (length < text.size())
        while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
    std::memcpy(out, text.data(), length);
}
}
Bridge::~Bridge() {
    stop=true;if(worker.joinable())worker.join();
    if(socket!=BAD_SOCKET)closeSocket(socket);
}
bool Bridge::start() {
    if(socket!=BAD_SOCKET)return true;
    if(!socketsReady())return false;
    socket=::socket(AF_INET,SOCK_DGRAM,0);
    if(socket==BAD_SOCKET)return false;
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(socket,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0){closeSocket(socket);socket=BAD_SOCKET;return false;}
    nonblocking(socket);
    worker=std::thread([this]{
        while(!stop){
            ClockSyncPacket p{};sockaddr_in peer{};
#ifdef _WIN32
            int length=sizeof(peer);
#else
            socklen_t length=sizeof(peer);
#endif
            auto size=recvfrom(socket,reinterpret_cast<char*>(&p),sizeof(p),0,reinterpret_cast<sockaddr*>(&peer),&length);
            auto received=monotonicNowNs();
            if(size==sizeof(p) && !std::memcmp(p.magic,"GDCLK1\0",8) && p.version==1 && p.kind==1 &&
               p.sessionID==processSessionID() && peer.sin_addr.s_addr==htonl(INADDR_LOOPBACK)){
                p.kind=2;p.t2=received;p.t3=monotonicNowNs();
                sendto(socket,reinterpret_cast<const char*>(&p),sizeof(p),0,reinterpret_cast<sockaddr*>(&peer),length);
            } else {
                fd_set readable;FD_ZERO(&readable);FD_SET(socket,&readable);timeval timeout{};timeout.tv_usec=2000;
                select(int(socket)+1,&readable,nullptr,nullptr,&timeout);
            }
        }
    });
    return true;
}
void Bridge::publish(const Snapshot& s) {
    if(socket==BAD_SOCKET)return;
    SongLinkPacket p;p.flags=(s.enabled?1:0)|(s.playing?2:0)|(s.looping?4:0);
    p.epoch=s.epoch;p.attempt=s.attempt;p.position=s.position;p.rate=s.rate;p.offset=s.offset;
    p.musicVolume=s.musicVolume;p.effectsVolume=s.effectsVolume;
    p.timestamp=s.timestamp?s.timestamp:monotonicNowNs();p.sessionID=processSessionID();
    p.loopStart=s.loopStart;p.loopEnd=s.loopEnd;
    p.channelID=s.channelID;p.triggerGain=s.triggerGain;p.fadeCount=std::min(s.fadeCount,8u);
    std::copy_n(s.fades.begin(),p.fadeCount,p.fades);
    copyText(p.status,s.status);
    copyText(p.level,s.level);
    copyText(p.song,s.song);
    if(s.path.size()<sizeof(p.path))std::strncpy(p.path,s.path.c_str(),sizeof(p.path)-1);else p.flags&=~2u;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(receiverPort());address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    sendto(socket,reinterpret_cast<const char*>(&p),sizeof(p),0,reinterpret_cast<sockaddr*>(&address),sizeof(address));
}
Bridge& bridge(){static Bridge b;return b;}
}
