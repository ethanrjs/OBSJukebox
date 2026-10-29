#include "Updater.hpp"
#include "UpdateCrypto.hpp"
#include "UpdateKey.hpp"
#include "UpdatePlatform.hpp"
#include "UpdateRules.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/utils/web.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace geode::prelude;
namespace ur = update_rules;

namespace {
constexpr auto RELEASES = "https://github.com/ethanrjs/OBSJukebox/releases";
constexpr uint64_t MAX_SETUP = 200ull * 1024 * 1024;

std::string baseUrl() {
#ifdef OBS_JUKEBOX_QA
    if (auto url = std::getenv("OBS_JUKEBOX_UPDATE_BASE_URL")) return url;
#endif
    return RELEASES;
}
std::array<uint8_t, 65> publicKey() {
#ifdef OBS_JUKEBOX_QA
    if (auto hex = std::getenv("OBS_JUKEBOX_UPDATE_PUBLIC_KEY"))
        if (auto key = ur::parsePublicKeyHex(hex)) return *key;
#endif
    return UPDATE_PUBLIC_KEY;
}
ur::Platform platform() {
#if defined(GEODE_IS_WINDOWS)
    return update_platform::underWine() ? ur::Platform::Other : ur::Platform::Windows;
#elif defined(GEODE_IS_ARM_MAC)
    return ur::Platform::MacArm64;
#else
    return ur::Platform::Other;
#endif
}

// Paths, fixed on the main thread before any background work starts.
struct Paths {
    std::filesystem::path staging, result, reopenFlag;
};
Paths paths;

struct Offer {
    ur::Manifest manifest;
    std::string bytes;
    ByteVector signature;
};
enum class Panel { None, Ask, Link, Progress };
// Main-thread state.
Panel panel = Panel::None;
ur::Version panelVersion;
std::string progressText;
std::optional<Offer> offer;
std::vector<std::string> pendingNotices;
bool launched = false;
// Counts main menus. GD builds a new MenuLayer each time the player returns to it, so a changed count
// means the player left the menu at least once since an update attempt started.
unsigned menuGeneration = 0;

ur::Version running() {
    auto v = Mod::get()->getVersion();
    return {static_cast<uint32_t>(v.getMajor()), static_cast<uint32_t>(v.getMinor()), static_cast<uint32_t>(v.getPatch())};
}
bool onMainMenu() {
    auto scene = CCDirector::get()->getRunningScene();
    return scene && scene->getChildByType<MenuLayer>(0);
}
// Notifications appear only on the main menu; others wait for the next visit.
void notify(std::string text) {
    if (onMainMenu()) Notification::create(text, NotificationIcon::Info, 5.f)->show();
    else pendingNotices.push_back(std::move(text));
}

std::optional<ur::Version> savedVersion(const char* key) { return ur::parseVersion(Mod::get()->getSavedValue<std::string>(key, "")); }
void saveVersion(const char* key, std::optional<ur::Version> v) { Mod::get()->setSavedValue<std::string>(key, v ? ur::toString(*v) : ""); }
ur::Saved loadSaved() {
    return {savedVersion("update-blocked-version"), savedVersion("update-attempts-version"),
            static_cast<int>(Mod::get()->getSavedValue<int64_t>("update-attempts", 0)), savedVersion("last-notified-version")};
}
void store(const ur::Saved& saved) {
    saveVersion("update-blocked-version", saved.blocked);
    saveVersion("update-attempts-version", saved.attemptsVersion);
    Mod::get()->setSavedValue<int64_t>("update-attempts", saved.attempts);
    saveVersion("last-notified-version", saved.notified);
}

bool authentic(std::string_view bytes, std::span<const uint8_t> signature) {
    return update_crypto::verifyP256(publicKey(), {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()}, signature);
}
// The staged manifest, only if its signature still holds.
std::optional<Offer> readStaged() {
    auto read = file::readString(paths.staging / "update.json");
    auto readSignature = file::readBinary(paths.staging / "update.json.sig");
    if (read.isErr() || readSignature.isErr()) return std::nullopt;
    auto bytes = read.unwrap();
    auto signature = readSignature.unwrap();
    if (signature.size() > 512 || !authentic(bytes, signature)) return std::nullopt;
    auto manifest = ur::parseManifest(bytes);
    if (!manifest) return std::nullopt;
    return Offer{*manifest, bytes, signature};
}
std::optional<ur::Result> readResult() {
    auto text = file::readString(paths.result);
    if (text.isErr()) return std::nullopt;
    auto parsed = matjson::parse(text.unwrap());
    if (parsed.isErr()) return std::nullopt;
    auto json = parsed.unwrap();
    auto version = ur::parseVersion(json["version"].asString().unwrapOr(""));
    auto status = ur::parseStatus(json["status"].asString().unwrapOr(""));
    bool repair = json["repair"].asBool().unwrapOr(false);
    if (!version || !status || (repair && *status == ur::Status::Waiting)) return std::nullopt;
    return ur::Result{*version, *status, repair, json["message"].asString().unwrapOr("")};
}
bool matches(const std::filesystem::path& file, const ur::FileEntry& entry) {
    std::error_code error;
    if (std::filesystem::file_size(file, error) != entry.size || error) return false;
    auto digest = update_crypto::sha256File(file);
    return digest && *digest == entry.sha256;
}
std::string userAgent() { return "OBSJukebox/" + ur::toString(running()); }

void refreshPanel();
void exitForUpdate(ur::Version version, unsigned startedOn);

// Downloads (or reuses) the setup for offer and saves its manifest next to it. Background thread.
std::optional<std::filesystem::path> stage(const Offer& o, std::function<void(float)> progress) {
    std::error_code error;
    if (update_platform::pathHasLink(paths.staging)) return std::nullopt;
    std::filesystem::create_directories(paths.staging, error);
    if (error || update_platform::pathHasLink(paths.staging)) return std::nullopt;
    auto entry = *ur::entryFor(o.manifest, platform());
    // The size is signed, so refuse an impossible setup before any request is made.
    if (entry.size > MAX_SETUP) return std::nullopt;
    auto name = ur::assetName(o.manifest.version, platform());
    auto setup = paths.staging / name;
    if (!matches(setup, entry)) {
        // Anything else in staging belongs to another version.
        // Collect first: removing entries while iterating is unspecified. This also deletes a setup that failed matches().
        std::vector<std::filesystem::path> old;
        for (auto& item : std::filesystem::directory_iterator(paths.staging, error)) old.push_back(item.path());
        for (auto& item : old) std::filesystem::remove_all(item, error);
        // rename() on Windows won't replace it. The error_code overload, since a throw here would end GD.
        bool remains = std::filesystem::exists(setup, error);
        if (error || remains) return std::nullopt;
        auto url = fmt::format("{}/download/v{}/{}", baseUrl(), ur::toString(o.manifest.version), name);
        auto head = web::WebRequest().userAgent(userAgent()).timeout(std::chrono::seconds(15)).transferBody(false).getSync(url);
        if (auto length = head.header("Content-Length")) {
            uint64_t size = 0;
            std::string_view text = *length;
            if (std::from_chars(text.data(), text.data() + text.size(), size).ec != std::errc{} || size != entry.size) return std::nullopt;
        }
        auto response = web::WebRequest().userAgent(userAgent()).timeout(std::chrono::seconds(600))
            .onProgress([progress](web::WebProgress const& p) { if (auto percent = p.downloadProgress()) progress(*percent); })
            .getSync(url);
        if (!response.ok() || response.data().size() > MAX_SETUP || response.data().size() != entry.size) return std::nullopt;
        auto part = setup;
        part += ".part";
        if (response.into(part).isErr() || !matches(part, entry)) { std::filesystem::remove(part, error); return std::nullopt; }
        std::filesystem::rename(part, setup, error);
        if (error) return std::nullopt;
    }
    if (file::writeString(paths.staging / "update.json", o.bytes).isErr() || file::writeBinary(paths.staging / "update.json.sig", o.signature).isErr()) return std::nullopt;
    return setup;
}

struct Launch {
    std::filesystem::path program;
    std::vector<std::string> args;
    update_platform::Job job = update_platform::Job::Free;
};
// Re-checks what is about to run and prepares its command. Background thread: it hashes
// the whole setup and, on macOS, unzips it. gdFolder comes from the main thread.
std::optional<Launch> prepareLaunch(ur::Version version, const std::filesystem::path& gdFolder) {
    auto staged = readStaged();
    if (!staged || staged->manifest.version != version) return std::nullopt;
    auto entry = ur::entryFor(staged->manifest, platform());
    auto setup = paths.staging / ur::assetName(version, platform());
    if (!entry || !matches(setup, *entry)) return std::nullopt;
    Launch launch{.job = update_platform::job()};
    if (launch.job == update_platform::Job::Trapped) return launch; // the caller shows the link panel
#if defined(GEODE_IS_WINDOWS)
    launch.program = setup;
    launch.args = {"--update", "--gd", utils::string::pathToString(gdFolder), "--result", utils::string::pathToString(paths.result),
                   "--reopen-flag", utils::string::pathToString(paths.reopenFlag)};
#elif defined(GEODE_IS_ARM_MAC)
    auto folder = paths.staging / fmt::format("{}-{:08x}", ur::toString(version), std::random_device{}());
    std::error_code folderError;
    // Reserve the folder first, so ditto can never merge into an existing tree.
    if (!std::filesystem::create_directory(folder, folderError) || folderError) return std::nullopt;
    if (!update_platform::unzip(setup, folder)) { std::filesystem::remove_all(folder, folderError); return std::nullopt; }
    launch.program = "/bin/zsh";
    launch.args = {(folder / "OBS Jukebox Setup.app/Contents/Resources/Install.command").string(), "--update", "--result", paths.result.string(),
                   "--reopen-flag", paths.reopenFlag.string(), gdFolder.string()};
#else
    return std::nullopt;
#endif
    return launch;
}

// "Update now", the "now" mode, and "After I close GD and OBS" all end here. Main thread.
void startUpdate(bool restart, bool viaNotice) {
    if (!offer) return;
    auto o = *offer;
    // Only the panel's "Update now" shows progress; the automatic mode shows no panel.
    bool showProgress = restart && !viaNotice;
    if (showProgress) { panel = Panel::Progress; panelVersion = o.manifest.version; progressText = "Downloading..."; refreshPanel(); }
    unsigned startedOn = menuGeneration;
#if defined(GEODE_IS_WINDOWS)
    auto gdFolder = dirs::getGameDir();
#elif defined(GEODE_IS_ARM_MAC)
    auto gdFolder = update_platform::gdBundle();
#else
    auto gdFolder = std::filesystem::path{};
#endif
    std::thread([o, restart, viaNotice, showProgress, startedOn, gdFolder] {
        // Redraw the panel only when the whole percentage changes, not on every network callback.
        auto shown = std::make_shared<std::atomic<int>>(-1);
        auto setup = stage(o, [showProgress, shown](float percent) {
            int whole = static_cast<int>(percent);
            if (!showProgress || shown->exchange(whole) == whole) return;
            Loader::get()->queueInMainThread([whole] { progressText = fmt::format("Downloading... {}%", whole); refreshPanel(); });
        });
        auto launch = setup ? prepareLaunch(o.manifest.version, gdFolder) : std::nullopt;
        Loader::get()->queueInMainThread([launch, restart, viaNotice, startedOn, version = o.manifest.version] {
            if (launch && launch->job == update_platform::Job::Trapped) {
                log::warn("GD's job object would end the update setup with GD; showing the download page instead");
                panel = Panel::Link; panelVersion = version; refreshPanel();
                return;
            }
            if (!launch || !update_platform::startSetup(launch->program, launch->args, launch->job == update_platform::Job::Breakaway)) {
                if (panel == Panel::Progress) panel = Panel::None;
                refreshPanel();
                notify("Update failed: the download or the setup didn't start.");
                return;
            }
            // A cold start of the setup takes several seconds; say so, or the panel looks stuck at 100%.
            if (panel == Panel::Progress) { progressText = "Starting the setup..."; refreshPanel(); }
            // Wait for the setup to say it is ready before reporting anything.
            std::thread([version, restart, viaNotice, startedOn] {
                bool ready = false;
                for (int i = 0; i < 40 && !ready; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    auto r = readResult();
                    ready = r && r->version == version && r->status == ur::Status::Waiting;
                }
                // A first run on a cold start can take longer than 10 seconds. If the setup holds its
                // lock it has started and will honour the wait; don't call that a failure.
                if (!ready && update_platform::setupWaiting()) ready = true;
                Loader::get()->queueInMainThread([ready, version, restart, viaNotice, startedOn] {
                    if (!ready) { panel = Panel::None; refreshPanel(); notify("Update failed: the setup didn't start."); return; }
                    if (!restart) {
                        notify(fmt::format("OBS Jukebox {} installs after you close GD and OBS.", ur::toString(version)));
                        return;
                    }
                    if (!viaNotice) {
                        progressText = "Restarting...";
                        refreshPanel();
                        exitForUpdate(version, startedOn);
                        return;
                    }
                    notify(fmt::format("Updating OBS Jukebox to {}. GD will restart shortly.", ur::toString(version)));
                    std::thread([version, startedOn] {
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                        Loader::get()->queueInMainThread([version, startedOn] { exitForUpdate(version, startedOn); });
                    }).detach();
                });
            }).detach();
        });
    }).detach();
}

// Leaves the game so the setup can install: only from the main menu, and only once the reopen flag is written.
void exitForUpdate(ur::Version version, unsigned startedOn) {
    panel = Panel::None;
    // The player asked for a restart, so GD comes back after the install even if they close GD themselves.
    if (file::writeString(paths.reopenFlag, "").isErr()) {
        refreshPanel();
        notify(fmt::format("OBS Jukebox {} installs after you close GD and OBS. GD won't reopen by itself.", ur::toString(version)));
        return;
    }
    // Leaving the menu at any point since the click, even after coming back, turns this into after-close.
    if (!onMainMenu() || menuGeneration != startedOn || update_platform::obsRunning()) {
        refreshPanel();
        notify(fmt::format("OBS Jukebox {} installs after you close GD and OBS. GD reopens by itself.", ur::toString(version)));
        return;
    }
    utils::game::exit(true);
}

void showLinkPanel(ur::Version version) { panel = Panel::Link; panelVersion = version; refreshPanel(); }

// Main thread, with a verified manifest from the check.
void decide(Offer o, bool obsRunning, ur::Mode mode) {
    auto saved = loadSaved();
    auto hasEntry = ur::entryFor(o.manifest, platform()).has_value();
    auto choice = ur::planOffer({mode, platform(), running(), o.manifest.version, hasEntry, obsRunning, onMainMenu(), saved});
    // A setup started in this job would die with GD. That can't be fixed later, so link to the download,
    // once per version; after that there is nothing more this launch can do.
    if (choice != ur::Offer::None && choice != ur::Offer::LinkPanel && platform() == ur::Platform::Windows
        && update_platform::job() == update_platform::Job::Trapped) {
        if (saved.notified == o.manifest.version) return;
        saved.notified = o.manifest.version;
        store(saved);
        showLinkPanel(o.manifest.version);
        return;
    }
    switch (choice) {
    case ur::Offer::None: return;
    case ur::Offer::LinkPanel: saved.notified = o.manifest.version; store(saved); showLinkPanel(o.manifest.version); return;
    case ur::Offer::AskPanel: offer = std::move(o); panel = Panel::Ask; panelVersion = offer->manifest.version; refreshPanel(); return;
    case ur::Offer::StartAfterClose: offer = std::move(o); startUpdate(false, false); return;
    case ur::Offer::StartNow: offer = std::move(o); startUpdate(true, true); return;
    }
}

// Background thread: fetch and verify the latest manifest.
void check(ur::Mode mode) {
    auto get = [](const std::string& url, size_t cap) -> std::optional<ByteVector> {
        auto response = web::WebRequest().userAgent(userAgent()).timeout(std::chrono::seconds(15)).getSync(url);
        if (!response.ok() || response.data().size() > cap) return std::nullopt;
        return response.data();
    };
    auto bytes = get(baseUrl() + "/latest/download/update.json", 16 * 1024);
    auto signature = get(baseUrl() + "/latest/download/update.json.sig", 512);
    if (!bytes || !signature) { log::info("No update information this launch"); return; }
    std::string text(bytes->begin(), bytes->end());
    if (!authentic(text, *signature)) { log::warn("update.json failed its signature check"); return; }
    auto manifest = ur::parseManifest(text);
    if (!manifest) { log::warn("update.json has an unexpected form"); return; }
    bool obs = update_platform::obsRunning();
    Loader::get()->queueInMainThread([o = Offer{*manifest, text, *signature}, obs, mode]() mutable { decide(std::move(o), obs, mode); });
}

void firstLaunch() {
    auto version = Mod::get()->getVersion();
    if (Mod::get()->getID() != "local.separate_song" || version.getTag()) return;
    auto mode = ur::parseMode(Mod::get()->getSettingValue<std::string>("update-mode"));
    if (mode == ur::Mode::Off) return;
    paths = {Mod::get()->getSaveDir() / "update", Mod::get()->getSaveDir() / "update-result.json", Mod::get()->getSaveDir() / "update-reopen-gd"};
    // A waiting setup owns staging and the result file; touch nothing.
    if (update_platform::setupWaiting()) return;
    if (update_platform::pathHasLink(paths.staging)) { log::warn("The update folder is a link; skipping updates this launch"); return; }
    std::error_code error;
    auto staged = readStaged();
    if (!staged) std::filesystem::remove_all(paths.staging, error);
    auto plan = ur::planLaunch({running(), false, staged ? std::optional(staged->manifest.version) : std::nullopt, readResult(), loadSaved()});
    if (plan.deleteReopenFlag) std::filesystem::remove(paths.reopenFlag, error);
    if (plan.deleteResult) std::filesystem::remove(paths.result, error);
    if (plan.deleteStaging) std::filesystem::remove_all(paths.staging, error);
    store(plan.saved);
    if (plan.notice != ur::Notice::None) notify(ur::noticeText(plan.notice, staged ? staged->manifest.version : running(), plan.message));
    if (plan.linkPanel) showLinkPanel(*plan.linkPanel);
    if (plan.stop) return;
    std::thread(check, mode).detach();
}

CCMenuItemSpriteExtra* button(const char* text, std::function<void()> action) {
    auto sprite = ButtonSprite::create(text, "goldFont.fnt", "GJ_button_01.png", .8f);
    sprite->setScale(.5f);
    return CCMenuItemExt::createSpriteExtra(sprite, [action](CCMenuItemSpriteExtra*) { action(); });
}
// A small panel in the top-right corner. It takes no input outside its own buttons.
void drawPanel(MenuLayer* menu) {
    if (auto old = menu->getChildByID("update-panel"_spr)) old->removeFromParent();
    if (panel == Panel::None) return;
    auto win = CCDirector::get()->getWinSize();
    auto root = CCNode::create();
    root->setID("update-panel"_spr);
    root->setContentSize({230.f, 52.f});
    root->setAnchorPoint({1.f, 1.f});
    root->setPosition({win.width - 4.f, win.height - 4.f});
    auto background = CCScale9Sprite::create("square02b_001.png");
    background->setColor({0, 0, 0});
    background->setOpacity(140);
    background->setContentSize(root->getContentSize());
    background->setAnchorPoint({0.f, 0.f});
    root->addChild(background);
    auto title = CCLabelBMFont::create(
        panel == Panel::Progress ? progressText.c_str() : fmt::format("OBS Jukebox {} is out.", ur::toString(panelVersion)).c_str(), "bigFont.fnt");
    title->limitLabelWidth(210.f, .4f, .1f);
    title->setPosition({115.f, 38.f});
    root->addChild(title);
    auto buttons = CCMenu::create();
    buttons->setContentSize({220.f, 22.f});
    buttons->setPosition({115.f, 15.f});
    buttons->setLayout(RowLayout::create()->setGap(4.f));
    auto close = [] { panel = Panel::None; refreshPanel(); };
    if (panel == Panel::Ask) {
        if (!update_platform::obsRunning()) buttons->addChild(button("Update now", [] {
            if (update_platform::obsRunning()) { refreshPanel(); return; }
            startUpdate(true, false);
        }));
        buttons->addChild(button("After I close GD and OBS", [] { panel = Panel::None; refreshPanel(); startUpdate(false, false); }));
        buttons->addChild(button("x", close));
    } else if (panel == Panel::Link) {
        buttons->addChild(button("Download page", [] { web::openLinkInBrowser(std::string(RELEASES) + "/latest"); panel = Panel::None; refreshPanel(); }));
        buttons->addChild(button("x", close));
    }
    buttons->updateLayout();
    root->addChild(buttons);
    menu->addChild(root, 100);
}
void refreshPanel() {
    auto scene = CCDirector::get()->getRunningScene();
    if (auto menu = scene ? scene->getChildByType<MenuLayer>(0) : nullptr) drawPanel(menu);
}
} // namespace

namespace updater {
void onMainMenu(MenuLayer* menu) {
    ++menuGeneration;
    if (!launched) { launched = true; firstLaunch(); }
    for (auto& text : pendingNotices) Notification::create(text, NotificationIcon::Info, 5.f)->show();
    pendingNotices.clear();
    drawPanel(menu);
}
} // namespace updater
