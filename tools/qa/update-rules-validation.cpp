#include "../../src/UpdateCrypto.hpp"
#include "../../src/UpdateRules.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace update_rules;
static unsigned checks = 0;
static void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}
static std::vector<uint8_t> bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
static std::string text(const std::filesystem::path& path) {
    auto data = bytes(path);
    return {data.begin(), data.end()};
}
static void rulesPart1(const std::filesystem::path& fixtures) {
    check(parseVersion("1.2.5") == Version{1, 2, 5}, "parses a release version");
    check(parseVersion("999999999.0.0").has_value(), "accepts nine-digit components");
    for (auto bad : {"1.2", "1.2.3.4", "v1.2.3", "1.2.3-beta", "1234567890.0.0", "1..3", "", "1.2.x"})
        check(!parseVersion(bad), "refuses a malformed version");
    check(Version{1, 2, 10} > Version{1, 2, 9} && Version{2, 0, 0} > Version{1, 99, 99}, "compares as three integers");
    check(toString({1, 2, 5}) == "1.2.5", "prints a version");

    auto manifestText = text(fixtures / "update.json");
    auto manifest = parseManifest(manifestText);
    check(manifest && manifest->version == Version{9, 8, 7} && manifest->windows && manifest->macArm64, "parses the canonical manifest");
    check(manifest->windows->size == 1 && manifest->macArm64->size == 2 && manifest->windows->sha256[0] == 0xbb, "reads sizes and hashes");
    auto both = [&](std::string body) { return parseManifest(body); };
    check(!both(std::string(16 * 1024 + 1, ' ')), "refuses a manifest over 16 KB");
    check(!both(manifestText.substr(0, manifestText.size() - 1)), "refuses a manifest without its newline");
    std::string schema2 = manifestText; schema2.replace(schema2.find("\"schema\":1"), 10, "\"schema\":2");
    check(!both(schema2), "refuses another schema");
    std::string extra = manifestText; extra.replace(extra.find("\"schema\""), 1, "\"x\":1,\"");
    check(!both(extra), "refuses an unknown field");
    std::string spaced = manifestText; spaced.replace(spaced.find(":"), 1, ": ");
    check(!both(spaced), "refuses a non-canonical form");
    std::string big = manifestText; big.replace(big.find("9.8.7"), 5, "1234567890.8.7");
    check(!both(big), "refuses a ten-digit version component");
    std::string onlyWindows = "{\"files\":{\"windows\":{\"sha256\":\"" + std::string(64, 'b') + "\",\"size\":1}},\"schema\":1,\"version\":\"1.0.0\"}\n";
    auto windowsOnly = parseManifest(onlyWindows);
    check(windowsOnly && windowsOnly->windows && !windowsOnly->macArm64 && !entryFor(*windowsOnly, Platform::MacArm64), "a missing platform has no entry");
    std::string danglingComma = "{\"files\":{\"macos-arm64\":{\"sha256\":\"" + std::string(64, 'a') + "\",\"size\":2},},\"schema\":1,\"version\":\"1.0.0\"}\n";
    check(!parseManifest(danglingComma), "refuses a comma without a second entry");
    check(!parseManifest("{\"files\":{},\"schema\":1,\"version\":\"1.0.0\"}\n"), "refuses a manifest with no files");
    check(assetName({1, 2, 5}, Platform::Windows) == "OBS-Jukebox-1.2.5-Windows-Setup.exe", "builds the Windows asset name");
    check(assetName({1, 2, 5}, Platform::MacArm64) == "OBS-Jukebox-1.2.5-macOS-arm64.zip", "builds the macOS asset name");

    for (auto name : {"plain.sig", "padded-r.sig", "padded-s.sig"})
        check(derToRaw(bytes(fixtures / name)).has_value(), "accepts an OpenSSL signature, padded or not");
    auto plain = bytes(fixtures / "plain.sig");
    auto mutate = [&](auto change) { auto copy = plain; change(copy); return derToRaw(copy).has_value(); };
    check(!mutate([](auto& d) { d.pop_back(); }), "refuses a truncated signature");
    check(!mutate([](auto& d) { d.push_back(0); }), "refuses trailing bytes");
    check([&] { auto d = plain; d.push_back(0); d[1] += 1; return derToRaw(d).has_value(); }() == false, "refuses a trailing byte inside the SEQUENCE");
    check(!mutate([](auto& d) { d[1] = 0x81; }), "refuses a long-form length");
    check(!mutate([](auto& d) { d[4] |= 0x80; }), "refuses a negative INTEGER");
    check(!mutate([](auto& d) { d[2] = 0x03; }), "refuses a non-INTEGER");
    std::vector<uint8_t> unneededPad{0x30, 0x08, 0x02, 0x02, 0x00, 0x01, 0x02, 0x02, 0x00, 0x01};
    check(!derToRaw(unneededPad), "refuses a 0x00 before a byte under 0x80");
    std::vector<uint8_t> tooLong(74, 0x01); tooLong[0] = 0x30; tooLong[1] = 72;
    check(!derToRaw(tooLong), "refuses a signature over 72 bytes");
    std::vector<uint8_t> wide{0x30, 0x26, 0x02, 0x21}; wide.insert(wide.end(), 33, 0x01); wide.insert(wide.end(), {0x02, 0x01, 0x01});
    check(!derToRaw(wide), "refuses a 33-byte INTEGER without a pad");
    std::vector<uint8_t> over{0x30, 0x27, 0x02, 0x22}; over.insert(over.end(), 34, 0x01); over.insert(over.end(), {0x02, 0x01, 0x01});
    check(!derToRaw(over), "refuses an INTEGER over 33 bytes");

    auto hex = text(fixtures / "test-public.hex");
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) hex.pop_back();
    auto key = parsePublicKeyHex(hex);
    check(key && (*key)[0] == 0x04, "parses a hex public key");
    check(!parsePublicKeyHex(hex.substr(2)) && !parsePublicKeyHex("zz" + hex.substr(2)), "refuses a malformed hex key");

    check(quoteWindowsArgument(L"C:\\Program Files\\GD") == L"\"C:\\Program Files\\GD\"", "quotes a path with spaces");
    check(quoteWindowsArgument(L"C:\\dir\\") == L"\"C:\\dir\\\\\"", "doubles a trailing backslash before the closing quote");
    check(quoteWindowsArgument(L"a\"b") == L"\"a\\\"b\"", "escapes a quote");
    check(quoteWindowsArgument(L"a\\\"b") == L"\"a\\\\\\\"b\"", "doubles backslashes before an escaped quote");
}
static void rulesPart2() {
    check(parseMode(MODE_ASK) == Mode::Ask && parseMode(MODE_AFTER_CLOSE) == Mode::AfterClose && parseMode(MODE_NOW) == Mode::Now && parseMode(MODE_OFF) == Mode::Off, "maps each shown setting value");
    check(parseMode("anything else") == Mode::Ask, "an unknown setting value asks");
    check(parseStatus("waiting") == Status::Waiting && parseStatus("failed") == Status::Failed && !parseStatus("done"), "parses result statuses");

    Version v124{1, 2, 4}, v125{1, 2, 5}, v126{1, 2, 6};
    auto launch = [&](LaunchInput in) { return planLaunch(in); };
    auto waiting = launch({v124, true, v125, Result{v125, Status::Waiting}, {}});
    check(waiting.stop && !waiting.deleteStaging && !waiting.deleteResult && !waiting.deleteReopenFlag, "a waiting setup stops everything, keeping its flag");
    auto installed = launch({v125, false, v125, Result{v125, Status::Installed}, {}});
    check(installed.notice == Notice::Installed && installed.deleteStaging && installed.deleteResult && !installed.stop, "an installed result shows once and cleans up");
    auto declined = launch({v124, false, v125, Result{v125, Status::Declined}, {}});
    check(declined.notice == Notice::Declined && declined.saved.attempts == 1 && declined.saved.attemptsVersion == v125 && !declined.deleteStaging, "a cancelled UAC prompt counts one attempt and keeps the staged file");
    Saved two{.attemptsVersion = v125, .attempts = 2};
    auto third = launch({v124, false, v125, Result{v125, Status::Failed, false, "disk full"}, two});
    check(third.notice == Notice::ThirdFailure && third.linkPanel == v125 && third.saved.notified == v125 && third.deleteStaging, "the third failure retires the version and shows the link panel");
    check(third.message == "disk full", "the plan carries the failure message");
    auto repair = launch({v124, false, v125, Result{v125, Status::Installed, true}, {}});
    check(repair.notice == Notice::Repair && repair.saved.blocked == v125 && repair.stop && repair.deleteStaging && repair.deleteResult, "repair overrides the status and blocks");
    check(launch({v124, false, v125, Result{v125, Status::Failed, true}, {}}).notice == Notice::Repair && launch({v124, false, v125, Result{v125, Status::Declined, true}, {}}).notice == Notice::Repair, "repair overrides a failed or declined result too");
    auto forged = launch({v124, false, v125, Result{Version{999, 0, 0}, Status::Failed, true}, {}});
    check(forged.notice == Notice::None && !forged.saved.blocked && forged.deleteResult && !forged.stop, "a result for another version is deleted unread");
    auto shutdown = launch({v124, false, v125, Result{v125, Status::Waiting}, {}});
    check(shutdown.notice == Notice::None && !shutdown.stop && !shutdown.deleteStaging, "a stale waiting result is like a shutdown");
    auto finished = launch({v125, false, v125, std::nullopt, {}});
    check(finished.deleteStaging && finished.notice == Notice::None, "an authentic staged manifest for the running version is cleanup");
    Saved blocked{.blocked = v125};
    check(launch({v125, false, std::nullopt, std::nullopt, blocked}).saved.blocked == v125, "reaching the blocked version doesn't clear the block");
    check(!launch({v126, false, std::nullopt, std::nullopt, blocked}).saved.blocked, "a newer version clears the block");

    OfferInput base{Mode::Ask, Platform::Windows, v124, v125, true, false, true, {}};
    auto offer = [&](auto change) { auto in = base; change(in); return planOffer(in); };
    check(offer([](auto&) {}) == Offer::AskPanel, "ask mode shows the panel");
    check(offer([](auto& in) { in.mode = Mode::AfterClose; }) == Offer::StartAfterClose, "after-close mode starts without a panel");
    check(offer([](auto& in) { in.mode = Mode::Now; }) == Offer::StartNow, "now mode restarts when OBS is closed on the main menu");
    check(offer([](auto& in) { in.mode = Mode::Now; in.obsRunning = true; }) == Offer::StartAfterClose, "now mode never closes OBS");
    check(offer([](auto& in) { in.mode = Mode::Now; in.onMainMenu = false; }) == Offer::StartAfterClose, "now mode never exits outside the main menu");
    check(offer([](auto& in) { in.mode = Mode::Off; }) == Offer::None, "off does nothing");
    check(offer([v124](auto& in) { in.latest = v124; }) == Offer::None, "an equal version isn't offered");
    check(offer([](auto& in) { in.platform = Platform::Other; }) == Offer::LinkPanel, "Linux gets the link panel");
    check(offer([](auto& in) { in.hasEntry = false; }) == Offer::LinkPanel, "a missing platform entry gets the link panel");
    check(offer([&](auto& in) { in.platform = Platform::Other; in.saved.notified = v125; }) == Offer::None, "the link panel shows once per version");
    check(offer([&](auto& in) { in.saved.attemptsVersion = v125; in.saved.attempts = 3; }) == Offer::None, "a retired version is never offered");
    check(offer([&](auto& in) { in.saved.blocked = v125; in.running = v125; in.latest = v126; }) == Offer::LinkPanel, "while blocked, only the link panel appears");
    check(offer([&](auto& in) { in.saved.blocked = v125; in.running = v124; in.latest = v125; }) == Offer::None, "the blocked version itself isn't offered");

    check(noticeText(Notice::Installed, v125) == "OBS Jukebox updated to 1.2.5.", "installed text");
    check(noticeText(Notice::Failed, v125) == "Update failed. It'll be offered again.", "failed text leaves the reason to the log");
}
static void cryptoChecks(const std::filesystem::path& fixtures) {
    auto hex = text(fixtures / "test-public.hex");
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) hex.pop_back();
    auto key = *parsePublicKeyHex(hex);
    auto message = bytes(fixtures / "update.json");
    for (auto name : {"plain.sig", "padded-r.sig", "padded-s.sig"})
        check(update_crypto::verifyP256(key, message, bytes(fixtures / name)), "a valid signature verifies, padded or not");
    auto changedMessage = message; changedMessage[10] ^= 1;
    check(!update_crypto::verifyP256(key, changedMessage, bytes(fixtures / "plain.sig")), "one changed manifest byte fails");
    auto changedSignature = bytes(fixtures / "plain.sig"); changedSignature.back() ^= 1;
    check(!update_crypto::verifyP256(key, message, changedSignature), "one changed signature byte fails");
    auto wrongKey = key; wrongKey[40] ^= 1;
    check(!update_crypto::verifyP256(wrongKey, message, bytes(fixtures / "plain.sig")), "the wrong key fails");
    auto truncated = bytes(fixtures / "plain.sig"); truncated.pop_back();
    check(!update_crypto::verifyP256(key, message, truncated), "a malformed signature fails before the OS sees it");
    auto digest = update_crypto::sha256File(fixtures / "update.json");
    check(digest.has_value() && (*digest)[0] == 0x92 && (*digest)[31] == 0xff, "the hash matches sha256sum");
    check(!update_crypto::sha256File(fixtures / "missing.file"), "a missing file has no hash");
}
int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: update-rules-validation fixtures-folder\n"; return 2; }
    rulesPart1(argv[1]);
    rulesPart2();
    cryptoChecks(argv[1]);
    std::cout << checks << " update rule checks passed\n";
}
