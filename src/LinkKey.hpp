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
#ifdef _WIN32
#include "Socket.hpp"
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#endif

namespace link_key {
// SHA-256 as FIPS 180-4 defines it.
class Sha256 {
    std::array<uint32_t, 8> state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                  0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block[64]{};
    size_t used = 0;
    uint64_t total = 0;
    static uint32_t rotate(uint32_t x, int n) { return x >> n | x << (32 - n); }
    void compress() {
        static constexpr uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
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
