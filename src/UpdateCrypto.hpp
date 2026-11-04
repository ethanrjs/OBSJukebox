#pragma once
// Signature checks through the operating system's own crypto: BCrypt on Windows, Security on macOS.
// sha256File hashes with link_key::Sha256, shared with the link key, so both work the same everywhere.
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace update_crypto {
// ECDSA P-256 with SHA-256 over message. The DER must pass update_rules::derToRaw first.
bool verifyP256(const std::array<uint8_t, 65>& publicKey, std::span<const uint8_t> message, std::span<const uint8_t> der);
std::optional<std::array<uint8_t, 32>> sha256File(const std::filesystem::path& path);
} // namespace update_crypto
