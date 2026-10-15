// ctest builds this in Release, and these asserts are the checks.
#undef NDEBUG
#include "../../../src/AudioDownmix.hpp"
#include "../../../src/LinkKey.hpp"
#include "../../../src/LinkPacket.hpp"
#include "../../../src/Socket.hpp"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#ifndef _WIN32
#include <sys/resource.h>
#endif

static std::string hex(const link_key::Sha256::Digest &digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (auto byte : digest)
        text += {digits[byte >> 4], digits[byte & 15]};
    return text;
}
static std::string sha256(const std::string &data) {
    link_key::Sha256 hash;
    hash.update(data.data(), data.size());
    return hex(hash.finish());
}
static std::string hmac(const std::string &key, const std::string &data) {
    link_key::HmacSha256 mac(key.data(), key.size());
    mac.update(data.data(), data.size());
    return hex(mac.finish());
}
// Reference values from Python's hashlib and hmac: FIPS 180-4 examples and RFC 4231 cases 1, 2 and 6.
static void linkKeyChecks() {
    assert(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    assert(sha256(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    assert(hmac(std::string(20, '\x0b'), "Hi There") ==
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    assert(hmac("Jefe", "what do ya want for nothing?") ==
           "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    assert(hmac(std::string(131, '\xaa'), "Test Using Larger Than Block-Size Key - Hash Key First") ==
           "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    // A tag covers every byte but itself, and only the same key reproduces it.
    SongLinkPacket packet;
    packet.position = 2.5;
    signPacket(processLinkKey(), packet);
    assert(signedWith(processLinkKey(), packet));
    auto changed = packet;
    changed.position = 2.75;
    assert(!signedWith(processLinkKey(), changed));
    auto other = processLinkKey();
    other[31] ^= 1;
    assert(!signedWith(other, packet));
    // The key file holds exactly 64 lowercase hex digits.
    auto file = std::filesystem::temp_directory_path() / "obs-jukebox-key-check" / "link-key";
    assert(writeLinkKey(processLinkKey(), file) && readLinkKey(file) == processLinkKey());
    for (const char *bad : {"", "0123", "zz", "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef"}) {
        std::ofstream(file, std::ios::trunc) << bad;
        assert(!readLinkKey(file));
    }
    std::ofstream(file, std::ios::trunc) << std::string(64, 'a') << "\r\n";
    assert(readLinkKey(file) && (*readLinkKey(file))[0] == 0xaa);
    std::filesystem::remove_all(file.parent_path());
}

int main() {
    linkKeyChecks();
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
    std::cout << "PASS: SHA-256 and HMAC vectors, link key file, downmix, QA isolation/cache, socket wait" << std::endl;
}
