#include "UpdateCrypto.hpp"
#include "UpdateRules.hpp"
#include <algorithm>
#include <fstream>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#endif

namespace update_crypto {
#ifdef _WIN32
namespace {
bool ok(NTSTATUS status) { return status >= 0; }
struct Algorithm {
    BCRYPT_ALG_HANDLE handle = nullptr;
    ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Key {
    BCRYPT_KEY_HANDLE handle = nullptr;
    ~Key() { if (handle) BCryptDestroyKey(handle); }
};
struct Hash {
    BCRYPT_HASH_HANDLE handle = nullptr;
    ~Hash() { if (handle) BCryptDestroyHash(handle); }
};
} // namespace

bool verifyP256(const std::array<uint8_t, 65>& publicKey, std::span<const uint8_t> message, std::span<const uint8_t> der) {
    auto raw = update_rules::derToRaw(der);
    if (!raw || publicKey[0] != 0x04) return false;
    std::array<uint8_t, 32> digest{};
    if (!ok(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(message.data()), static_cast<ULONG>(message.size()), digest.data(), 32))) return false;
    Algorithm algorithm;
    if (!ok(BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return false;
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    auto header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::copy(publicKey.begin() + 1, publicKey.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    Key key;
    if (!ok(BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key.handle, blob.data(), static_cast<ULONG>(blob.size()), 0))) return false;
    return ok(BCryptVerifySignature(key.handle, nullptr, digest.data(), 32, raw->data(), 64, 0));
}
std::optional<std::array<uint8_t, 32>> sha256File(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    Hash hash;
    if (!ok(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash.handle, nullptr, 0, nullptr, 0, 0))) return std::nullopt;
    std::vector<char> buffer(1 << 20);
    while (file) {
        file.read(buffer.data(), buffer.size());
        if (file.gcount() && !ok(BCryptHashData(hash.handle, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(file.gcount()), 0))) return std::nullopt;
    }
    if (file.bad()) return std::nullopt;
    std::array<uint8_t, 32> out{};
    if (!ok(BCryptFinishHash(hash.handle, out.data(), 32, 0))) return std::nullopt;
    return out;
}
#else
namespace {
struct Cf {
    CFTypeRef ref = nullptr;
    ~Cf() { if (ref) CFRelease(ref); }
};
} // namespace

bool verifyP256(const std::array<uint8_t, 65>& publicKey, std::span<const uint8_t> message, std::span<const uint8_t> der) {
    if (!update_rules::derToRaw(der) || publicKey[0] != 0x04) return false;
    Cf keyData{CFDataCreate(nullptr, publicKey.data(), publicKey.size())};
    const void* keys[] = {kSecAttrKeyType, kSecAttrKeyClass};
    const void* values[] = {kSecAttrKeyTypeECSECPrimeRandom, kSecAttrKeyClassPublic};
    Cf attributes{CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    Cf key{SecKeyCreateWithData(static_cast<CFDataRef>(keyData.ref), static_cast<CFDictionaryRef>(attributes.ref), nullptr)};
    if (!key.ref) return false;
    Cf data{CFDataCreate(nullptr, message.data(), message.size())};
    Cf signature{CFDataCreate(nullptr, der.data(), der.size())};
    return SecKeyVerifySignature(static_cast<SecKeyRef>(const_cast<void*>(key.ref)), kSecKeyAlgorithmECDSASignatureMessageX962SHA256,
                                 static_cast<CFDataRef>(data.ref), static_cast<CFDataRef>(signature.ref), nullptr);
}
std::optional<std::array<uint8_t, 32>> sha256File(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<char> buffer(1 << 20);
    while (file) {
        file.read(buffer.data(), buffer.size());
        if (file.gcount()) CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(file.gcount()));
    }
    if (file.bad()) return std::nullopt;
    std::array<uint8_t, 32> out{};
    CC_SHA256_Final(out.data(), &context);
    return out;
}
#endif
} // namespace update_crypto
