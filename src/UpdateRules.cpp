#include "UpdateRules.hpp"
#include <algorithm>
#include <charconv>
#include <regex>

namespace update_rules {
std::optional<Version> parseVersion(std::string_view text) {
    uint32_t parts[3];
    for (int i = 0; i < 3; ++i) {
        auto end = i < 2 ? text.find('.') : text.size();
        if (end == std::string_view::npos || end == 0 || end > 9) return std::nullopt;
        auto digits = text.substr(0, end);
        if (!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
        std::from_chars(digits.data(), digits.data() + digits.size(), parts[i]);
        text.remove_prefix(i < 2 ? end + 1 : end);
    }
    if (!text.empty()) return std::nullopt;
    return Version{parts[0], parts[1], parts[2]};
}
std::string toString(Version v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

static std::optional<std::array<uint8_t, 32>> hash(std::string_view hex) {
    std::array<uint8_t, 32> out{};
    for (size_t i = 0; i < 32; ++i)
        if (std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, out[i], 16).ec != std::errc{}) return std::nullopt;
    return out;
}
std::optional<Manifest> parseManifest(std::string_view bytes) {
    if (bytes.size() > 16 * 1024) return std::nullopt;
    // The re(...)re delimiter, because the pattern itself contains )" sequences.
    static const std::regex shape(
        R"re(\{"files":\{(?:"macos-arm64":\{"sha256":"([0-9a-f]{64})","size":(0|[1-9][0-9]{0,11})\}(,?))?)re"
        R"re((?:"windows":\{"sha256":"([0-9a-f]{64})","size":(0|[1-9][0-9]{0,11})\})?\},"schema":1,)re"
        R"re("version":"([0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9})"\}\n)re");
    std::cmatch m;
    if (!std::regex_match(bytes.data(), bytes.data() + bytes.size(), m, shape)) return std::nullopt;
    // The comma belongs between two entries, and only there.
    if (m[3].length() != (m[1].matched && m[4].matched ? 1 : 0)) return std::nullopt;
    // A manifest with no file at all is not a valid release.
    if (!m[1].matched && !m[4].matched) return std::nullopt;
    Manifest out{*parseVersion(m[6].str()), {}, {}};
    auto entry = [](const std::csub_match& sha, const std::csub_match& size) {
        FileEntry e{*hash(sha.str()), std::stoull(size.str())};
        return e;
    };
    if (m[1].matched) out.macArm64 = entry(m[1], m[2]);
    if (m[4].matched) out.windows = entry(m[4], m[5]);
    return out;
}
std::optional<FileEntry> entryFor(const Manifest& manifest, Platform platform) {
    if (platform == Platform::Windows) return manifest.windows;
    if (platform == Platform::MacArm64) return manifest.macArm64;
    return std::nullopt;
}
std::string assetName(Version version, Platform platform) {
    return "OBS-Jukebox-" + toString(version) + (platform == Platform::Windows ? "-Windows-Setup.exe" : "-macOS-arm64.zip");
}

std::optional<std::array<uint8_t, 64>> derToRaw(std::span<const uint8_t> der) {
    if (der.size() < 8 || der.size() > 72 || der[0] != 0x30 || der[1] != der.size() - 2) return std::nullopt;
    std::array<uint8_t, 64> raw{};
    size_t at = 2;
    for (size_t half = 0; half < 2; ++half) {
        if (at + 2 > der.size() || der[at] != 0x02) return std::nullopt;
        size_t length = der[at + 1];
        at += 2;
        if (length < 1 || length > 33 || at + length > der.size()) return std::nullopt;
        auto value = der.subspan(at, length);
        at += length;
        if (value[0] & 0x80) return std::nullopt; // negative
        if (value[0] == 0) {                       // allowed only before a byte with its top bit set
            if (value.size() == 1 || !(value[1] & 0x80)) return std::nullopt;
            value = value.subspan(1);
        }
        if (value.size() > 32) return std::nullopt;
        std::copy(value.begin(), value.end(), raw.begin() + half * 32 + (32 - value.size()));
    }
    if (at != der.size()) return std::nullopt;
    return raw;
}
std::optional<std::array<uint8_t, 65>> parsePublicKeyHex(std::string_view hex) {
    if (hex.size() != 130) return std::nullopt;
    std::array<uint8_t, 65> key{};
    for (size_t i = 0; i < 65; ++i) {
        auto [end, error] = std::from_chars(hex.data() + i * 2, hex.data() + i * 2 + 2, key[i], 16);
        if (error != std::errc{} || end != hex.data() + i * 2 + 2) return std::nullopt;
    }
    if (key[0] != 0x04) return std::nullopt;
    return key;
}
std::wstring quoteWindowsArgument(std::wstring_view argument) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

Mode parseMode(std::string_view shown) {
    if (shown == MODE_AFTER_CLOSE) return Mode::AfterClose;
    if (shown == MODE_NOW) return Mode::Now;
    if (shown == MODE_OFF) return Mode::Off;
    return Mode::Ask;
}
std::optional<Status> parseStatus(std::string_view text) {
    if (text == "waiting") return Status::Waiting;
    if (text == "installed") return Status::Installed;
    if (text == "declined") return Status::Declined;
    if (text == "failed") return Status::Failed;
    return std::nullopt;
}
// Kept to one short line: a toast doesn't wrap, and a longer one runs off the screen.
std::string noticeText(Notice notice, Version version) {
    switch (notice) {
    case Notice::Installed: return "OBS Jukebox updated to " + toString(version) + ".";
    case Notice::Declined: return "OBS Jukebox update cancelled.";
    case Notice::Failed: return "Update failed. It'll be offered again.";
    case Notice::ThirdFailure: return "Update failed again. Use the download page.";
    case Notice::Repair: return "Update failed. Reinstall from the download page.";
    case Notice::None: break;
    }
    return {};
}

static void showLinkPanel(LaunchPlan& plan, Version version) {
    if (plan.saved.notified == version) return;
    plan.linkPanel = version;
    plan.saved.notified = version;
}
LaunchPlan planLaunch(const LaunchInput& in) {
    LaunchPlan plan;
    plan.saved = in.saved;
    if (in.setupWaiting) { plan.stop = true; return plan; }
    plan.deleteReopenFlag = true;
    // Every result is read once, so the file goes whether it parsed or not.
    plan.deleteResult = true;
    if (plan.saved.blocked && in.running > *plan.saved.blocked) plan.saved.blocked.reset();
    // A result counts only for the authentic staged version; anything else is deleted unread.
    auto result = in.result && in.staged && in.result->version == *in.staged ? in.result : std::nullopt;
    if (result && result->repair) {
        plan.notice = Notice::Repair;
        plan.saved.blocked = result->version;
        plan.deleteStaging = true;
        plan.stop = true;
        showLinkPanel(plan, result->version);
        return plan;
    }
    if (in.staged && *in.staged <= in.running) {
        if (result && result->status == Status::Installed && result->version == in.running) plan.notice = Notice::Installed;
        plan.deleteStaging = true;
        return plan;
    }
    if (result && (result->status == Status::Declined || result->status == Status::Failed)) {
        int count = plan.saved.attemptsVersion == result->version ? plan.saved.attempts + 1 : 1;
        plan.message = result->message;
        plan.saved.attemptsVersion = result->version;
        plan.saved.attempts = count;
        if (count >= 3) {
            plan.notice = Notice::ThirdFailure;
            plan.deleteStaging = true;
            showLinkPanel(plan, result->version);
        } else {
            plan.notice = result->status == Status::Declined ? Notice::Declined : Notice::Failed;
        }
    }
    return plan;
}
Offer planOffer(const OfferInput& in) {
    if (in.mode == Mode::Off || in.latest <= in.running) return Offer::None;
    auto link = in.saved.notified == in.latest ? Offer::None : Offer::LinkPanel;
    if (in.saved.blocked && in.running <= *in.saved.blocked) return in.latest > *in.saved.blocked ? link : Offer::None;
    if (in.saved.attemptsVersion == in.latest && in.saved.attempts >= 3) return Offer::None;
    if (in.platform == Platform::Other || !in.hasEntry) return link;
    if (in.mode == Mode::AfterClose) return Offer::StartAfterClose;
    if (in.mode == Mode::Now) return in.obsRunning || !in.onMainMenu ? Offer::StartAfterClose : Offer::StartNow;
    return Offer::AskPanel;
}
} // namespace update_rules
