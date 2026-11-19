#pragma once
// Signature checks through the OS's own crypto: BCrypt on Windows, Security on macOS. sha256File
// hashes with link_key::Sha256, shared with the link key, so both work the same everywhere.
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>

namespace update_crypto {
// ECDSA P-256 with SHA-256 over message. The DER must pass update_rules::derToRaw first.
bool verifyP256(
    std::array<uint8_t, 65> const& publicKey, std::span<uint8_t const> message,
    std::span<uint8_t const> der
);
std::optional<std::array<uint8_t, 32>> sha256File(std::filesystem::path const& path);
}
