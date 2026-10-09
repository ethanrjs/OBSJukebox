#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SongSocket = SOCKET;
inline constexpr SongSocket BAD_SOCKET = INVALID_SOCKET;
inline bool socketsReady() {
    static bool ready = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
}
inline void closeSocket(SongSocket s) { closesocket(s); }
inline void nonblocking(SongSocket s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
using SongSocket = int;
inline constexpr SongSocket BAD_SOCKET = -1;
inline bool socketsReady() { return true; }
inline void closeSocket(SongSocket s) { close(s); }
inline void nonblocking(SongSocket s) { fcntl(s, F_SETFL, O_NONBLOCK); }
#endif

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
