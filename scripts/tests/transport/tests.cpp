#include "../../../src/AudioDownmix.hpp"
#include "../../../src/LinkPacket.hpp"
#include "../../../src/Socket.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#ifndef _WIN32
#include <sys/resource.h>
#endif

int main() {
    using separate_song::downmixStereo;
    float mono[] = {.4f};
    assert(downmixStereo(mono, 1)[0] == .4f && downmixStereo(mono, 1)[1] == .4f);
    float stereo[] = {.2f, .3f};
    assert(downmixStereo(stereo, 2)[0] == .2f && downmixStereo(stereo, 2)[1] == .3f);
    float quad[] = {0, 0, 1, 0};
    assert(std::abs(downmixStereo(quad, 4)[0] - .70710678f) < .00001f);
    assert(downmixStereo(quad, 4)[1] == 0);
    float center[] = {0, 0, 1, 0, 0, 0};
    auto mixed = downmixStereo(center, 6);
    assert(mixed[0] > .7f && mixed[0] == mixed[1]);
    float rear[] = {0, 0, 0, 0, 0, 0, 0, 1};
    assert(downmixStereo(rear, 8)[0] == 0 && downmixStereo(rear, 8)[1] > .7f);
    float lfe[] = {0, 0, 0, 1, 0, 0};
    assert(downmixStereo(lfe, 6)[0] == 0 && downmixStereo(lfe, 6)[1] == 0);
#ifdef _WIN32
    _putenv_s("OBS_JUKEBOX_TEST_PORT", "49876");
#else
    setenv("OBS_JUKEBOX_TEST_PORT", "49876", 1);
#endif
#ifdef OBS_JUKEBOX_QA
    assert(receiverPort() == 49876);
#else
    assert(receiverPort() == SONG_LINK_PORT);
#endif
    int cached = receiverPort();
#ifdef _WIN32
    _putenv_s("OBS_JUKEBOX_TEST_PORT", "49877");
#else
    setenv("OBS_JUKEBOX_TEST_PORT", "49877", 1);
#endif
    assert(receiverPort() == cached);
    assert(socketsReady());
    auto listener = socket(AF_INET, SOCK_DGRAM, 0);
    assert(listener != BAD_SOCKET);
#ifndef _WIN32
    // This descriptor could corrupt memory with FD_SET in the old implementation.
    rlimit limit{};
    assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    if (limit.rlim_cur < 2049) {
        limit.rlim_cur = 2049;
        assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
    }
    auto high = fcntl(listener, F_DUPFD, 2048);
    assert(high >= 2048);
    closeSocket(listener);
    listener = high;
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
#ifdef _WIN32
    int length = sizeof(endpoint);
#else
    socklen_t length = sizeof(endpoint);
#endif
    assert(getsockname(listener, reinterpret_cast<sockaddr *>(&endpoint), &length) == 0);
    assert(!waitSocketReadable(listener, 1));
    auto sender = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sendto(sender, "x", 1, 0, reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint)) == 1);
    assert(waitSocketReadable(listener, 100));
    closeSocket(sender);
    closeSocket(listener);
    std::cout << "PASS: downmix, QA isolation/cache, socket wait" << std::endl;
}
