#pragma once
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <mswsock.h>
using SongSocket = SOCKET;
inline constexpr SongSocket BAD_SOCKET = INVALID_SOCKET;

inline bool socketsReady() {
    static bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}

inline void closeSocket(SongSocket s) {
    closesocket(s);
}

inline void nonblocking(SongSocket s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

// Windows reports a datagram sent to a port that later closed as an error on the NEXT recvfrom.
// These sockets talk to processes that come and go, so tell Winsock not to surface that yet.
inline void ignoreConnReset(SongSocket s) {
    DWORD behavior = 0, returned = 0;
    WSAIoctl(
        s, SIO_UDP_CONNRESET, &behavior, sizeof(behavior), nullptr, 0, &returned, nullptr, nullptr
    );
}
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
using SongSocket = int;
inline constexpr SongSocket BAD_SOCKET = -1;

inline bool socketsReady() {
    return true;
}

inline void closeSocket(SongSocket s) {
    close(s);
}

inline void nonblocking(SongSocket s) {
    fcntl(s, F_SETFL, O_NONBLOCK);
}

// POSIX has no such option. These sockets never connect() and never set IP_RECVERR, so Linux
// normally does not report a closed peer on receive; the receivers tolerate any receive error anyway.
inline void ignoreConnReset(SongSocket) {}
#endif

// Returns a nonblocking UDP socket bound to 127.0.0.1, so it never listens on other interfaces, or
// BAD_SOCKET on failure.
inline SongSocket openLoopbackSocket() {
    if (!socketsReady()) return BAD_SOCKET;
    auto udp = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp == BAD_SOCKET) return BAD_SOCKET;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(udp, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        closeSocket(udp);
        return BAD_SOCKET;
    }
    nonblocking(udp);
    ignoreConnReset(udp);
    return udp;
}

// POSIX poll accepts descriptors above FD_SETSIZE; Windows fd_set stores handles.
inline bool waitSocketReadable(SongSocket socket, int milliseconds) {
#ifdef _WIN32
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = (milliseconds % 1000) * 1000;
    return select(0, &readable, nullptr, nullptr, &timeout) > 0;
#else
    pollfd descriptor{socket, POLLIN, 0};
    return poll(&descriptor, 1, milliseconds) > 0 && (descriptor.revents & POLLIN);
#endif
}
