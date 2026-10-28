#pragma once
// Updater decisions with no Geode or OS calls, so QA tests can build them. Spec:
// artifacts/specs/2026-10-27-auto-updater-design.md.
#include <array>
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace update_rules {
struct Version {
    uint32_t major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
// "1.2.5": three components of 1 to 9 digits, nothing else.
std::optional<Version> parseVersion(std::string_view text);
std::string toString(Version version);

enum class Platform { Windows, MacArm64, Other };
struct FileEntry {
    std::array<uint8_t, 32> sha256{};
    uint64_t size = 0;
};
struct Manifest {
    Version version;
    std::optional<FileEntry> windows, macArm64;
};
// Accepts only the exact bytes scripts/make-update-manifest.py writes; any other form fails.
std::optional<Manifest> parseManifest(std::string_view bytes);
std::optional<FileEntry> entryFor(const Manifest& manifest, Platform platform);
std::string assetName(Version version, Platform platform);

// OpenSSL's DER ECDSA signature as the 64-byte r||s form BCrypt verifies, or nothing when the DER
// isn't minimal: SEQUENCE { INTEGER r, INTEGER s }, short lengths, no trailing bytes.
std::optional<std::array<uint8_t, 64>> derToRaw(std::span<const uint8_t> der);
// 130 hex digits of an uncompressed P-256 point, for QA builds' test key.
std::optional<std::array<uint8_t, 65>> parsePublicKeyHex(std::string_view hex);
// One argument quoted for CreateProcessW, by the rules CommandLineToArgvW reads back.
std::wstring quoteWindowsArgument(std::wstring_view argument);

inline constexpr const char* MODE_ASK = "Ask me";
inline constexpr const char* MODE_AFTER_CLOSE = "Install after I close GD and OBS";
inline constexpr const char* MODE_NOW = "Install right away";
inline constexpr const char* MODE_OFF = "Don't check";
enum class Mode { Ask, AfterClose, Now, Off };
// The setting stores its shown text; anything unexpected asks.
Mode parseMode(std::string_view shown);

enum class Status { Waiting, Installed, Declined, Failed };
std::optional<Status> parseStatus(std::string_view text);
struct Result {
    Version version;
    Status status = Status::Waiting;
    bool repair = false;
    std::string message;
};
// The mod's saved values (spec 6.3, 4.4).
struct Saved {
    std::optional<Version> blocked;          // update-blocked-version
    std::optional<Version> attemptsVersion;  // update-attempts-version
    int attempts = 0;                        // update-attempts
    std::optional<Version> notified;         // last-notified-version
};
enum class Notice { None, Installed, Declined, Failed, ThirdFailure, Repair };
std::string noticeText(Notice notice, Version version, std::string_view message);

struct LaunchInput {
    Version running;
    bool setupWaiting = false;
    std::optional<Version> staged; // an authentic staged manifest's version
    std::optional<Result> result;
    Saved saved;
};
struct LaunchPlan {
    bool stop = false; // no check this launch
    Notice notice = Notice::None;
    std::optional<Version> linkPanel;
    bool deleteStaging = false, deleteResult = false, deleteReopenFlag = false;
    Saved saved;
    std::string message; // a failed result's message, for noticeText
};
// Spec 6.3 steps 1 and 2.
LaunchPlan planLaunch(const LaunchInput& in);

enum class Offer { None, LinkPanel, AskPanel, StartAfterClose, StartNow };
struct OfferInput {
    Mode mode = Mode::Ask;
    Platform platform = Platform::Other;
    Version running, latest;
    bool hasEntry = false, obsRunning = false, onMainMenu = false;
    Saved saved;
};
// Spec 6.3 step 4 and 4.1. A LinkPanel result means the caller saves latest as notified.
Offer planOffer(const OfferInput& in);
} // namespace update_rules
