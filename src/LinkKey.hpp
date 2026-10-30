#pragma once
// A random key the mod makes at launch and writes to a file in this user's own app data folder. The key
// itself never travels: every packet carries an HMAC-SHA256 tag made with it, so the plugin follows only
// this user's mod, and a program that can see loopback traffic still can't forge a packet.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#elif defined(__aarch64__)
#include <arm_neon.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif
#endif
#ifdef _WIN32
#include "Socket.hpp"
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#endif

namespace link_key {
inline constexpr uint32_t roundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#if defined(__x86_64__) || defined(_M_X64)
#define LINK_KEY_SHA_INSTRUCTIONS
#if defined(__GNUC__) || defined(__clang__)
#define LINK_KEY_SHA_TARGET __attribute__((target("sha,sse4.1")))
#else
#define LINK_KEY_SHA_TARGET
#endif
// Status packets are tagged on GD's main thread, effects packets on AudioTap's sender and clock
// replies on Bridge's worker; the plugin checks every one. The x86 SHA extensions hash a block
// several times faster than the portable rounds.
inline bool hasShaInstructions() {
    static const bool supported = [] {
#ifdef _MSC_VER
        int info[4];
        __cpuid(info, 0);
        if (info[0] < 7)
            return false;
        __cpuid(info, 1);
        const unsigned features = unsigned(info[2]);
        __cpuidex(info, 7, 0);
        const unsigned extended = unsigned(info[1]);
#else
        unsigned a = 0, b = 0, features = 0, d = 0, extended = 0, c = 0;
        if (!__get_cpuid(1, &a, &b, &features, &d) || !__get_cpuid_count(7, 0, &a, &extended, &c, &d))
            return false;
#endif
        const bool ssse3 = features & (1u << 9), sse41 = features & (1u << 19), sha = extended & (1u << 29);
        return ssse3 && sse41 && sha;
    }();
    return supported;
}
// One block with the SHA extensions, as in Intel's reference code: the state is held as ABEF and
// CDGH, and each step runs four rounds and extends the message schedule by four words.
LINK_KEY_SHA_TARGET inline void compressWithShaInstructions(uint32_t *state, const uint8_t *block) {
    const __m128i byteSwap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    const __m128i cdab = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(state)), 0xB1);
    const __m128i efgh = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(state + 4)), 0x1B);
    __m128i abef = _mm_alignr_epi8(cdab, efgh, 8), cdgh = _mm_blend_epi16(efgh, cdab, 0xF0);
    const __m128i abefBefore = abef, cdghBefore = cdgh;
    __m128i words[4];
    for (int i = 0; i < 4; ++i)
        words[i] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(block + i * 16)), byteSwap);
    for (int step = 0; step < 16; ++step) {
        const __m128i current = words[step & 3];
        const __m128i scheduled =
            _mm_add_epi32(current, _mm_loadu_si128(reinterpret_cast<const __m128i *>(roundConstants + step * 4)));
        cdgh = _mm_sha256rnds2_epu32(cdgh, abef, scheduled);
        if (step >= 3 && step < 15) {
            auto &next = words[(step + 1) & 3];
            next = _mm_sha256msg2_epu32(_mm_add_epi32(next, _mm_alignr_epi8(current, words[(step - 1) & 3], 4)),
                                        current);
        }
        abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(scheduled, 0x0E));
        if (step >= 1 && step < 13)
            words[(step - 1) & 3] = _mm_sha256msg1_epu32(words[(step - 1) & 3], current);
    }
    abef = _mm_add_epi32(abef, abefBefore);
    cdgh = _mm_add_epi32(cdgh, cdghBefore);
    const __m128i feba = _mm_shuffle_epi32(abef, 0x1B), dchg = _mm_shuffle_epi32(cdgh, 0xB1);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(state), _mm_blend_epi16(feba, dchg, 0xF0));
    _mm_storeu_si128(reinterpret_cast<__m128i *>(state + 4), _mm_alignr_epi8(dchg, feba, 8));
}
#elif defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__))
#define LINK_KEY_SHA_INSTRUCTIONS
#if defined(__ARM_FEATURE_SHA2)
#define LINK_KEY_SHA_TARGET // Apple's arm64 default CPU has them
#elif defined(__clang__)
#define LINK_KEY_SHA_TARGET __attribute__((target("sha2")))
#else
#define LINK_KEY_SHA_TARGET __attribute__((target("+sha2")))
#endif
// Every Apple Silicon CPU has the ARMv8 SHA-256 instructions; other arm64 CPUs may not, so the OS is
// asked either way.
inline bool hasShaInstructions() {
    static const bool supported = [] {
#ifdef __APPLE__
        int present = 0;
        size_t size = sizeof(present);
        return sysctlbyname("hw.optional.arm.FEAT_SHA256", &present, &size, nullptr, 0) == 0 && present != 0;
#else
        return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#endif
    }();
    return supported;
}
// One block with the ARMv8 SHA-256 instructions: the state stays in order as ABCD and EFGH, each step
// runs four rounds, and the first twelve steps extend the message schedule by four words.
LINK_KEY_SHA_TARGET inline void compressWithShaInstructions(uint32_t *state, const uint8_t *block) {
    uint32x4_t abcd = vld1q_u32(state), efgh = vld1q_u32(state + 4);
    const uint32x4_t abcdBefore = abcd, efghBefore = efgh;
    uint32x4_t words[4];
    for (int i = 0; i < 4; ++i)
        words[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + i * 16)));
    for (int step = 0; step < 16; ++step) {
        const uint32x4_t scheduled = vaddq_u32(words[step & 3], vld1q_u32(roundConstants + step * 4));
        const uint32x4_t abcdBeforeStep = abcd;
        abcd = vsha256hq_u32(abcd, efgh, scheduled);
        efgh = vsha256h2q_u32(efgh, abcdBeforeStep, scheduled);
        if (step < 12)
            words[step & 3] = vsha256su1q_u32(vsha256su0q_u32(words[step & 3], words[(step + 1) & 3]),
                                              words[(step + 2) & 3], words[(step + 3) & 3]);
    }
    vst1q_u32(state, vaddq_u32(abcd, abcdBefore));
    vst1q_u32(state + 4, vaddq_u32(efgh, efghBefore));
}
#else
inline bool hasShaInstructions() { return false; }
#endif
// SHA-256 as FIPS 180-4 defines it.
class Sha256 {
    std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64]{};
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rotate(uint32_t x, int n) { return x >> n | x << (32 - n); }
    void compress() {
#ifdef LINK_KEY_SHA_INSTRUCTIONS
        if (!portableOnly && hasShaInstructions())
            return compressWithShaInstructions(state.data(), block);
#endif
        const auto &k = roundConstants;
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 | uint32_t(block[i * 4 + 2]) << 8 |
                   block[i * 4 + 3];
        for (int i = 16; i < 64; ++i)
            w[i] = w[i - 16] + (rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
                   (rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ w[i - 2] >> 10);
        auto [a, b, c, d, e, f, g, h] = state;
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        uint32_t next[8] = {a, b, c, d, e, f, g, h};
        for (int i = 0; i < 8; ++i)
            state[i] += next[i];
    }

  public:
    using Digest = std::array<uint8_t, 32>;
    // Tests set this to compare the portable rounds with the SHA instructions; leave it false elsewhere.
    bool portableOnly = false;
    void update(const void *data, size_t size) {
        auto bytes = static_cast<const uint8_t *>(data);
        total += size;
        while (size) {
            size_t n = (std::min)(size, sizeof(block) - used); // windows.h may define min
            std::memcpy(block + used, bytes, n);
            used += n, bytes += n, size -= n;
            if (used == sizeof(block)) {
                compress();
                used = 0;
            }
        }
    }
    Digest finish() {
        uint64_t bits = total * 8;
        block[used++] = 0x80;
        if (used > 56) {
            std::fill(block + used, block + 64, uint8_t(0));
            compress();
            used = 0;
        }
        std::fill(block + used, block + 56, uint8_t(0));
        for (int i = 0; i < 8; ++i)
            block[56 + i] = uint8_t(bits >> (56 - 8 * i));
        compress();
        Digest digest;
        for (int i = 0; i < 32; ++i)
            digest[i] = uint8_t(state[i / 4] >> (24 - 8 * (i % 4)));
        return digest;
    }
};
// HMAC-SHA256 as RFC 2104 defines it.
class HmacSha256 {
    Sha256 inner, outer;

  public:
    HmacSha256(const void *key, size_t size) {
        uint8_t padded[64]{};
        if (size > sizeof(padded)) {
            Sha256 hashed;
            hashed.update(key, size);
            auto digest = hashed.finish();
            std::memcpy(padded, digest.data(), digest.size());
        } else
            std::memcpy(padded, key, size);
        uint8_t pad[64];
        for (int i = 0; i < 64; ++i)
            pad[i] = padded[i] ^ 0x36;
        inner.update(pad, sizeof(pad));
        for (int i = 0; i < 64; ++i)
            pad[i] = padded[i] ^ 0x5c;
        outer.update(pad, sizeof(pad));
    }
    void update(const void *data, size_t size) { inner.update(data, size); }
    Sha256::Digest finish() {
        auto digest = inner.finish();
        outer.update(digest.data(), digest.size());
        return outer.finish();
    }
};
} // namespace link_key

using LinkKey = std::array<uint8_t, 32>;
inline constexpr const char *LINK_KEY_NAME = "link-key";

inline const LinkKey &processLinkKey() {
    static const LinkKey key = [] {
        std::random_device random;
        LinkKey k{};
        for (auto &byte : k)
            byte = uint8_t(random());
        return k;
    }();
    return key;
}
// The tag is the first 16 bytes of the HMAC of the first `size` bytes of the packet, leaving out the
// tag field itself.
template <class Packet> void linkTag(const LinkKey &key, const Packet &packet, size_t size, uint8_t (&tag)[16]) {
    constexpr size_t at = offsetof(Packet, tag), after = at + sizeof(Packet::tag);
    auto bytes = reinterpret_cast<const uint8_t *>(&packet);
    link_key::HmacSha256 mac(key.data(), key.size());
    mac.update(bytes, at);
    if (size > after)
        mac.update(bytes + after, size - after);
    auto digest = mac.finish();
    std::memcpy(tag, digest.data(), sizeof(tag));
}
template <class Packet> void signPacket(const LinkKey &key, Packet &packet, size_t size = sizeof(Packet)) {
    linkTag(key, packet, size, packet.tag);
}
// Compares in constant time, so the time a check takes says nothing about the tag.
template <class Packet> bool signedWith(const LinkKey &key, const Packet &packet, size_t size = sizeof(Packet)) {
    uint8_t expected[16];
    linkTag(key, packet, size, expected);
    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(expected); ++i)
        difference |= expected[i] ^ packet.tag[i];
    return difference == 0;
}
// The file the mod writes, or an empty path if this user's app data folder can't be found. The Linux
// plugin looks for it inside the game's Wine prefix instead (LinuxPaths::keyFiles).
inline std::filesystem::path linkKeyFile() {
#ifdef OBS_JUKEBOX_QA
    if (auto configured = std::getenv("OBS_JUKEBOX_TEST_KEY_FILE"))
        return configured;
#endif
#ifdef _WIN32
    PWSTR folder = nullptr;
    std::filesystem::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder)))
        root = folder;
    CoTaskMemFree(folder);
    return root.empty() ? root : root / "OBS Jukebox" / LINK_KEY_NAME;
#elif defined(__APPLE__)
    auto home = std::getenv("HOME");
    return home ? std::filesystem::path(home) / "Library/Application Support/OBS Jukebox" / LINK_KEY_NAME
                : std::filesystem::path{};
#else
    return {};
#endif
}
// Writes the key as 64 hex digits. Only this user can read the file: Windows gives the app data folder
// to this user alone, and on macOS and Linux the file is made owner-only.
inline bool writeLinkKey(const LinkKey &key, const std::filesystem::path &file = linkKeyFile()) {
    if (file.empty())
        return false;
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        static constexpr char digits[] = "0123456789abcdef";
        for (auto byte : key)
            out << digits[byte >> 4] << digits[byte & 15];
        out << '\n';
        if (!out)
            return false;
    }
#ifndef _WIN32
    std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 error);
#endif
    return true;
}
// The key in a file the mod wrote: exactly 64 hex digits, then an optional line ending.
inline std::optional<LinkKey> readLinkKey(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    char text[67]{};
    in.read(text, sizeof(text));
    auto length = size_t(in.gcount());
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        --length;
    if (length != 64)
        return std::nullopt;
    auto value = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    LinkKey key{};
    for (size_t i = 0; i < key.size(); ++i) {
        int high = value(text[i * 2]), low = value(text[i * 2 + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        key[i] = uint8_t(high << 4 | low);
    }
    return key;
}
