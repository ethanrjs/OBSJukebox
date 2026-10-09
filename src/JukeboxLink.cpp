#include "JukeboxLink.hpp"
#include "PlaybackIdentity.hpp"
#include <Geode/utils/Keyboard.hpp>
#include <jukebox/ui/list/nong_cell.hpp>
#include <jukebox/events/start_download.hpp>
#include <jukebox/events/song_download_finished.hpp>
#include <jukebox/events/song_download_failed.hpp>
#include <jukebox/events/nong_deleted.hpp>
#include <jukebox/events/manual_song_added.hpp>
#include <jukebox/events/song_state_changed.hpp>
#include <jukebox/events/indexes_loaded.hpp>
#include <unordered_map>
#include <chrono>
#include <filesystem>
#include <cctype>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include <functional>
#include <Geode/modify/CCNode.hpp>

using namespace geode::prelude;
namespace separate_song::jukebox_link {
namespace {
using Clock = std::chrono::steady_clock;
struct Choice {
    int id = 0, offset = 0;
    std::string uid, name, path;
    bool original = false;
};
struct Request { bool pending = false; std::string error; Clock::time_point started{}; };
std::unordered_map<std::string, Request> requests;
std::unordered_map<std::string, Choice> addedSongs;
struct CachedManifest { matjson::Value json; Clock::time_point read{}; };
std::unordered_map<int, CachedManifest> manifests;
struct CachedPlayback {
    std::optional<Choice> obs;
    Choice game;
    std::string normalizedGamePath;
    bool obsReady = false, gameReady = false;
    Clock::time_point read{};
};
std::unordered_map<int, CachedPlayback> playback;
void invalidate(int id) { playback.erase(id); if (auto it = manifests.find(id); it != manifests.end()) it->second.read = {}; }

struct CellAccess : jukebox::NongCell {
    static int id(jukebox::NongCell* c) { return c->*(&CellAccess::m_songID); }
    static std::string uid(jukebox::NongCell* c) { return c->*(&CellAccess::m_uniqueID); }
    static bool original(jukebox::NongCell* c) { return c->*(&CellAccess::m_isDefault); }
    static jukebox::NongCellUI* ui(jukebox::NongCell* c) { return (c->*(&CellAccess::m_nongCell)).data(); }
    static std::optional<jukebox::index::IndexSongMetadata*> index(jukebox::NongCell* c) {
        return c->*(&CellAccess::m_indexSongMetadataOpt);
    }
};
struct UIAccess : jukebox::NongCellUI {
    static CCMenu* buttons(jukebox::NongCellUI* ui) { return ui->*(&UIAccess::m_buttonsMenu); }
    static CCMenuItemSpriteExtra* selected(jukebox::NongCellUI* ui) { return ui->*(&UIAccess::m_selectButton); }
    static CCNode* songInfo(jukebox::NongCellUI* ui) { return ui->*(&UIAccess::m_songInfoNode); }
};
// Rows where Jukebox hides its Game button (e.g. "Download nongs") get no OBS checkbox
// and can't be picked for OBS by right-click.
bool gameButtonShown(jukebox::NongCellUI* ui) {
    auto gameCheck = UIAccess::selected(ui);
    return gameCheck && gameCheck->isVisible();
}
std::filesystem::path utf8Path(std::string_view path) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
}
// Filesystem and JSON work never runs on the game/sampling thread. Results are
// copied under a short lock; failed manifest reads retain the last good value.
class FileCache {
    struct Entry {
        bool exists = false;
        matjson::Value json;
        bool pending = false;
        Clock::time_point checked{};
    };
    std::mutex mutex;
    std::condition_variable wake;
    std::unordered_map<std::string, Entry> entries;
    std::deque<std::pair<std::string, bool>> jobs;
    bool stopping = false;
    std::thread worker;
    void run() {
        for (;;) {
            std::pair<std::string, bool> job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping) return;
                job = std::move(jobs.front()); jobs.pop_front();
            }
            bool present = false;
            std::optional<matjson::Value> parsed;
            try {
                auto path = utf8Path(job.first);
                if (job.second) {
                    auto result = file::readJson(path);
                    if (result && result.unwrap().isObject()) parsed = result.unwrap();
                } else {
                    std::error_code error;
                    present = std::filesystem::is_regular_file(path, error) &&
                        std::filesystem::file_size(path, error) > 0 && !error;
                }
            } catch (const std::exception&) { }
            std::lock_guard lock(mutex);
            auto& entry = entries[job.first];
            entry.exists = present;
            if (parsed) entry.json = std::move(*parsed);
            entry.pending = false;
            entry.checked = Clock::now();
        }
    }
public:
    FileCache() : worker([this] { run(); }) {}
    void shutdown() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }
    bool known(const std::string& path) {
        std::lock_guard lock(mutex);
        auto found = entries.find(path);
        return found != entries.end() && found->second.checked != Clock::time_point{};
    }
    std::pair<bool, matjson::Value> get(const std::string& path, bool json = false) {
        if (path.empty()) return {};
        std::lock_guard lock(mutex);
        auto& entry = entries[path];
        if (!stopping && !entry.pending && Clock::now() - entry.checked >= std::chrono::seconds(1)) {
            entry.pending = true;
            jobs.emplace_back(path, json);
            wake.notify_one();
        }
        return {entry.exists, entry.json};
    }
};
// Shutdown is explicit before the game exits, never joined under the DLL loader lock.
FileCache& files() { static auto cache = new FileCache(); return *cache; }
bool exists(const std::string& path) { return files().get(path).first; }
std::filesystem::path base() { return Loader::get()->getLoadedMod("fleym.nongd")->getSaveDir(); }
// CellAccess and UIAccess read Jukebox's protected fields using the vendored 3.8.0 headers.
// Geode lets the dependency update to any newer 3.x, so skip the row UI on other builds.
bool cellLayoutMatches() {
    static const bool matches = [] {
        auto dependency = Loader::get()->getLoadedMod("fleym.nongd");
        if (!dependency) return false;
        auto version = dependency->getVersion();
        bool ok = version.getMajor() == 3 && version.getMinor() == 8 && version.getPatch() == 0 && !version.getTag();
        if (!ok) log::warn("Jukebox {} is not 3.8.0; OBS Jukebox integration is disabled; following game audio", version.toVString());
        return ok;
    }();
    return matches;
}
std::string key(int id, const std::string& uid) { return fmt::format("{}:{}", id, uid); }
matjson::Value saved() { return Mod::get()->getSavedValue<matjson::Value>("obs-selections", matjson::Value::object()); }
std::optional<Choice> choice(int id) {
    auto data = saved(); auto k = std::to_string(id);
    if (!data.contains(k)) return std::nullopt;
    auto v = data[k];
    Choice c{id, static_cast<int>(v["offset"].asInt().unwrapOr(0)),
             v["uid"].asString().unwrapOr(""), v["name"].asString().unwrapOr("OBS song"),
             v["path"].asString().unwrapOr(""), v["original"].asBool().unwrapOr(false)};
    if (c.uid.empty()) return std::nullopt;
    return c;
}
void store(const Choice& c) {
    auto data = saved();
    data[std::to_string(c.id)] = matjson::makeObject({{"uid", c.uid}, {"name", c.name},
        {"path", c.path}, {"offset", c.offset}, {"original", c.original}});
    Mod::get()->setSavedValue("obs-selections", data);
    invalidate(c.id);
}
void clear(int id) {
    auto data = saved(); data.erase(std::to_string(id));
    Mod::get()->setSavedValue("obs-selections", data);
    invalidate(id);
}
matjson::Value manifest(int id, bool = false) {
    if (!cellLayoutMatches()) return {};
    auto& cache = manifests[id];
    auto result = files().get(string::pathToString(base()/"manifest"/fmt::format("{}.json", id)), true).second;
    if (result.isObject()) cache.json = std::move(result);
    cache.read = Clock::now();
    return cache.json;
}
matjson::Value find(const matjson::Value& data, const std::string& uid, bool original = false) {
    if (original || data["default"]["unique_id"].asString().unwrapOr("") == uid) return data["default"];
    for (const char* group : {"locals", "hosted", "youtube"}) {
        if (!data[group].isArray()) continue;
        for (const auto& v : data[group].asArray().unwrap())
            if (v["unique_id"].asString().unwrapOr("") == uid) return v;
    }
    return matjson::Value();
}
std::string pathFor(const matjson::Value& song) {
    auto path = song["path"].asString().unwrapOr("");
    if (!path.empty() && (exists(path) || !files().known(path))) return path;
    auto name = song["filename"].asString().unwrapOr("");
    if (!name.empty()) return string::pathToString(base()/"nongs"/utf8Path(name));
    return path;
}
std::string originalPath(int id) {
    if (id < 0) {
        auto name = LevelTools::getAudioFileName(-id-1);
        return CCFileUtils::get()->fullPathForFilename(name.c_str(), false);
    }
    auto name = string::pathToString(utf8Path(MusicDownloadManager::sharedState()->pathForSongFolder(id).c_str()) /
                                    fmt::format("{}.{}", id, id > 9999999 ? "ogg" : "mp3"));
    return CCFileUtils::get()->fullPathForFilename(name.c_str(), false);
}
Choice resolved(Choice c) {
    auto meta = find(manifest(c.id), c.uid, c.original);
    if (meta.isObject()) {
        c.name = meta["name"].asString().unwrapOr(c.name);
        c.offset = static_cast<int>(meta["offset"].asInt().unwrapOr(c.offset));
        auto p = pathFor(meta); if (!p.empty()) c.path = p;
    } else if (auto it = addedSongs.find(key(c.id,c.uid)); it != addedSongs.end()) {
        c.name = it->second.name; c.offset = it->second.offset;
        if (!it->second.path.empty()) c.path = it->second.path;
    }
    if (c.original && (c.path.empty() || (files().known(c.path) && !exists(c.path)))) c.path = originalPath(c.id);
    return c;
}
int songID(GJGameLevel* level) { return level->m_songID > 0 ? level->m_songID : -level->m_audioTrack-1; }
std::string normalizedMusicPath(const std::string& path) {
    if (path.empty()) return {};
    static std::unordered_map<std::string, std::string> normalized;
    if (auto found = normalized.find(path); found != normalized.end()) return found->second;
    if (normalized.size() >= 4096) normalized.clear();
    auto full = CCFileUtils::get()->fullPathForFilename(path.c_str(), false);
    try {
        auto value = string::pathToString(utf8Path(full).lexically_normal());
        std::replace(value.begin(), value.end(), '\\', '/');
#ifdef GEODE_IS_WINDOWS
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
#endif
        normalized[path] = value;
        return value;
    } catch (const std::filesystem::filesystem_error&) { return {}; }
}
std::unordered_map<std::string, std::pair<Choice, bool>> deferredDownloads;
std::string download(Choice c, bool retry) {
    c = resolved(c);
    if (exists(c.path)) { requests.erase(key(c.id, c.uid)); return ""; }
    auto k = key(c.id, c.uid);
    auto manifestPath = string::pathToString(base()/"manifest"/fmt::format("{}.json", c.id));
    if ((!c.path.empty() && !files().known(c.path)) || !files().known(manifestPath)) {
        deferredDownloads[k] = {c, retry};
        return "";
    }
    auto& request = requests[k];
    if (request.pending && Clock::now() - request.started < std::chrono::seconds(30)) return "";
    if (request.pending) request = {false, "Download timed out. Select the song to retry."};
    if (!request.error.empty() && !retry) return request.error;
    request = {true, "", Clock::now()};
    log::info("OBS song cache miss: {} ({})", c.name, c.uid);
    if (c.original && c.id > 0) MusicDownloadManager::sharedState()->downloadSong(c.id);
    else if (c.original) request = {false, "Original song file is unavailable."};
    else if ([&] {
        auto locals = manifest(c.id)["locals"];
        if (!locals.isArray()) return false;
        for (const auto& local : locals.asArray().unwrap())
            if (local["unique_id"].asString().unwrapOr("") == c.uid) return true;
        return false;
    }())
        request = {false, "Local song file is unavailable. Choose an existing file in Jukebox."};
    else jukebox::event::StartDownload().send({c.id, c.uid});
    return requests[k].error;
}
Choice active(int id) {
    auto m = manifest(id); auto uid = m["active"].asString().unwrapOr("");
    auto v = find(m, uid); bool original = uid.empty() || uid == m["default"]["unique_id"].asString().unwrapOr("");
    Choice c{id, static_cast<int>(v["offset"].asInt().unwrapOr(0)), uid,
        v["name"].asString().unwrapOr("In-game song"), pathFor(v), original};
    if (original && (c.path.empty() || (files().known(c.path) && !exists(c.path)))) c.path = originalPath(id);
    return c;
}
CachedPlayback& forPlayback(int id) {
    auto now = Clock::now();
    if (auto found = playback.find(id); found != playback.end() &&
        now-found->second.read < std::chrono::seconds(1)) return found->second;
    CachedPlayback value;
    value.read = now;
    value.game = active(id);
    value.gameReady = exists(value.game.path);
    value.normalizedGamePath = normalizedMusicPath(value.game.path);
    if (auto picked = choice(id)) {
        value.obs = resolved(*picked);
        value.obsReady = exists(value.obs->path);
        if (value.obsReady) requests.erase(key(id, value.obs->uid));
    }
    return playback.insert_or_assign(id, std::move(value)).first->second;
}

WeakRef<CCNode> visibleList;
CCNode* findList(CCNode* scene) {
    auto list = visibleList.lock();
    if (!list || !list->isRunning()) return nullptr;
    for (auto node = list.data(); node; node = node->getParent()) {
        if (!node->isVisible()) return nullptr;
        if (node == scene) return list.data();
    }
    return nullptr;
}
bool unobscured(CCNode* scene, CCNode* list) {
    CCNode* owner = list;
    while (owner->getParent() && owner->getParent() != scene) owner = owner->getParent();
    bool afterOwner = false;
    for (auto child : CCArrayExt<CCNode*>(scene->getChildren())) {
        if (child == owner) { afterOwner = true; continue; }
        if (child->isVisible() && typeinfo_cast<FLAlertLayer*>(child) &&
            (child->getZOrder() > owner->getZOrder() || (afterOwner && child->getZOrder() == owner->getZOrder()))) return false;
    }
    return true;
}
std::vector<jukebox::NongCell*> cells(CCNode* list) {
    std::vector<jukebox::NongCell*> out;
    auto scroll = list->getChildByID("list");
    if (!scroll || !scroll->getChildrenCount()) return out;
    auto content = scroll->getChildByID("content-layer");
    if (!content) {
        if (auto layer = typeinfo_cast<ScrollLayer*>(scroll)) content = layer->m_contentLayer;
    }
    if (!content) return out;
    for (auto child : CCArrayExt<CCNode*>(content->getChildren()))
        if (auto cell = typeinfo_cast<jukebox::NongCell*>(child)) out.push_back(cell);
    return out;
}
void select(jukebox::NongCell* cell);
class OBSRowControl : public CCNode {
    jukebox::NongCell* m_cell = nullptr;
    Ref<CCMenuItemSpriteExtra> m_checkbox;
    CCSprite* m_sprite = nullptr;
    CCLabelBMFont* m_gameLabel = nullptr;
    CCLabelBMFont* m_obsLabel = nullptr;
    bool m_checked = false;
public:
    static OBSRowControl* create(jukebox::NongCell* cell) {
        auto control = new OBSRowControl();
        if (!control->init()) { delete control; return nullptr; }
        control->autorelease(); control->m_cell = cell;
        control->setID("control"_spr); control->setZOrder(20);
        control->m_sprite = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        control->m_sprite->setScale(.7f);
        control->m_checkbox = CCMenuItemSpriteExtra::create(control->m_sprite, control, menu_selector(OBSRowControl::onSelect));
        control->m_checkbox->setID("checkbox"_spr);
        control->m_checkbox->setContentSize({30.f, 30.f});
        control->m_sprite->setPosition({15.f, 15.f});
        control->m_gameLabel = CCLabelBMFont::create("Game", "bigFont.fnt");
        control->m_gameLabel->setID("game-label"_spr);
        control->m_gameLabel->setScale(.25f);
        control->addChild(control->m_gameLabel);
        control->m_obsLabel = CCLabelBMFont::create("OBS", "bigFont.fnt");
        control->m_obsLabel->setID("obs-label"_spr);
        control->m_obsLabel->setScale(.25f);
        control->addChild(control->m_obsLabel);
        control->scheduleUpdate();
        return control;
    }
    // Jukebox rebuilds the row's buttons menu when the Game song changes, dropping the checkbox.
    // The scheduler runs before drawing, so re-syncing here restores it in the same frame.
    // A rebuild destroys the old menu, which clears the checkbox's parent, so these two checks
    // catch every rebuild. Otherwise the row is still as sync() left it.
    void update(float) override {
        auto ui = CellAccess::ui(m_cell); if (!ui) return;
        if (m_checkbox->getParent() == UIAccess::buttons(ui) && m_checkbox->isVisible() == gameButtonShown(ui)) return;
        sync(ui, m_checked);
    }
    void onExit() override {
        // The checkbox lives in Jukebox's menu; detach it before its target dies.
        m_checkbox->setEnabled(false);
        m_checkbox->removeFromParent();
        CCNode::onExit();
    }
    void onEnter() override {
        CCNode::onEnter();
        m_checkbox->setEnabled(true);
    }
    void onSelect(CCObject*) { if (m_cell && isRunning()) select(m_cell); }
    void sync(jukebox::NongCellUI* ui, bool checked) {
        auto menu = UIAccess::buttons(ui); auto gameCheck = UIAccess::selected(ui);
        if (!menu || !gameCheck) return;
        // The menu skips invisible children, so hidden rows keep Jukebox's own layout.
        bool shown = gameButtonShown(ui);
        bool relayout = false;
        if (m_checkbox->getParent() != menu) {
            m_checkbox->removeFromParent();
            menu->insertAfter(m_checkbox.data(), gameCheck);
            relayout = true;
        }
        if (m_checkbox->isVisible() != shown) { m_checkbox->setVisible(shown); relayout = true; }
        // The flag lives on the menu but also covers the song info node: Jukebox 3.8.0's build()
        // recreates both together and only sets the Game button's visibility there.
        if (shown && !menu->getUserFlag("widened"_spr)) {
            menu->setUserFlag("widened"_spr);
            menu->setContentWidth(menu->getContentWidth()+35.f);
            if (auto info = UIAccess::songInfo(ui)) {
                info->setContentWidth(std::max(0.f, info->getContentWidth()-35.f));
                info->updateLayout();
            }
            relayout = true;
        }
        if (relayout) {
            menu->updateLayout();
            m_gameLabel->setPosition(convertToNodeSpace(menu->convertToWorldSpace(gameCheck->getPosition()))+CCPoint{0,-23});
            m_gameLabel->setVisible(shown);
            m_obsLabel->setPosition(convertToNodeSpace(menu->convertToWorldSpace(m_checkbox->getPosition()))+CCPoint{0,-23});
            m_obsLabel->setVisible(shown);
        }
        if (checked != m_checked) {
            m_checked = checked;
            m_sprite->setDisplayFrame(CCSpriteFrameCache::sharedSpriteFrameCache()->spriteFrameByName(
                checked ? "GJ_checkOn_001.png" : "GJ_checkOff_001.png"));
            m_sprite->setPosition({15.f, 15.f});
        }
    }
};
void paint(jukebox::NongCell* cell) {
    auto ui = CellAccess::ui(cell); if (!ui) return;
    bool checked = false;
    if (auto c = choice(CellAccess::id(cell)))
        checked = c->uid == CellAccess::uid(cell) || (c->original && CellAccess::original(cell));
    auto control = static_cast<OBSRowControl*>(cell->getChildByID("control"_spr));
    if (!control) { control = OBSRowControl::create(cell); if (!control) return; cell->addChild(control); }
    control->sync(ui, checked);
}
void select(jukebox::NongCell* cell) {
    auto ui = CellAccess::ui(cell); if (!ui) return;
    Choice c; c.id = CellAccess::id(cell); c.uid = CellAccess::uid(cell); c.original = CellAccess::original(cell);
    c.name = ui->m_songName;
    if (auto old = choice(c.id); old && (old->uid == c.uid || (old->original && c.original))) {
        clear(c.id); Notification::create("OBS selection cleared", NotificationIcon::Info)->show(); return;
    }
    manifest(c.id, true);
    if (auto index = CellAccess::index(cell)) {
        c.offset = (*index)->startOffset;
        c.path = string::pathToString(base()/"nongs"/fmt::format("{}-{}.mp3", (*index)->parentID->m_id, c.uid));
    }
    c = resolved(c); store(c);
    auto error = download(c, true);
    Notification::create(error.empty() ? "OBS song selected" : "OBS download failed", error.empty()?NotificationIcon::Success:NotificationIcon::Error)->show();
    if (!error.empty()) FLAlertLayer::create("OBS Song", error, "OK")->show();
    refreshUI();
}
}

void shutdown() { files().shutdown(); }
void refreshUI() {
    static auto lastRequestCheck = Clock::time_point{};
    if (Clock::now() - lastRequestCheck >= std::chrono::seconds(1)) {
        lastRequestCheck = Clock::now();
        for (auto& [key, request] : requests)
            if (request.pending && Clock::now() - request.started >= std::chrono::seconds(30))
                request = {false, "Download timed out. Select the song to retry."};
    }
    if (!deferredDownloads.empty()) {
        auto pending = std::move(deferredDownloads);
        deferredDownloads.clear();
        for (auto& [key, entry] : pending) download(entry.first, entry.second);
    }
    if (auto play = PlayLayer::get(); play && !play->m_isPaused) return;
    if (!cellLayoutMatches()) return;
    auto scene = CCDirector::get()->getRunningScene(); if (!scene) return;
    auto list = findList(scene); if (!list) return;
    for (auto cell : cells(list)) paint(cell);
}
void observeList(CCNode* node) {
    if (cellLayoutMatches() && node->getID() == "NongList") visibleList = node;
}
Readiness prepare(GJGameLevel* level, bool retry) {
    if (!Mod::get()->getSettingValue<bool>("enabled") || !level || !cellLayoutMatches()) return {};
    int id = songID(level);
    if (retry) invalidate(id);
    auto cached = forPlayback(id);
    // GD owns its own downloads. An unavailable OBS replacement never gates Play.
    if (!cached.obs || cached.obsReady) return {};
    if (retry) requests.erase(key(id, cached.obs->uid));
    auto error = download(*cached.obs, retry);
    // A cache probe in flight is not evidence that a local file is missing.
    if (!cached.obs->path.empty() && !files().known(cached.obs->path)) return {false, ""};
    return {false, error.empty() ? "OBS replacement unavailable" : error};
}
LevelSongs snapshotSongs(GJGameLevel* level) {
    int id = songID(level);
    return {id, levelSongIDs(id, std::string(level->m_songIDs))};
}
void fill(Snapshot& state, const LevelSongs* songs, const MusicSource& source) {
    state.path.clear(); state.song.clear();
    if (!songs) return;
    if (!cellLayoutMatches()) {
        state.path = source.path;
        state.song = "In-game song";
        return;
    }
    int id = songs->initialID;
    if (source.channel) {
        if (source.path.empty()) return;
        const auto sampledPath = normalizedMusicPath(source.path);
        std::optional<int> matched;
        bool ambiguous = false;
        auto consider = [&](int candidate) {
            if (forPlayback(candidate).normalizedGamePath != sampledPath) return;
            if (matched && *matched != candidate) ambiguous = true;
            else matched = candidate;
        };
        for (int candidate : songs->ids) consider(candidate);
        for (int candidate : source.extraIDs) consider(candidate);
        if (ambiguous) matched.reset();
        if (!matched) {
            state.song = "In-game song";
            auto full = CCFileUtils::get()->fullPathForFilename(source.path.c_str(), false);
            state.path = full;
            return;
        }
        id = *matched;
    }
    const auto& cached = forPlayback(id);
    if ((!cached.obs || !cached.obsReady) && source.channel) {
        state.path = source.path;
        state.song = "In-game song";
        return;
    }
    const auto& obs = cached.obs ? *cached.obs : cached.game;
    state.song = obs.name;
    if (cached.obs ? cached.obsReady : cached.gameReady) state.path = obs.path;
    state.offset += (obs.offset-(state.musicPosition?cached.game.offset:0))/1000.0;
}
void initialize() {
    if (!cellLayoutMatches()) return;
    jukebox::event::IndexesLoaded().listen([] {
        playback.clear(); manifests.clear();
        for (auto& [key, request] : requests)
            if (!request.pending) request.error.clear();
    }).leak();
    jukebox::event::SongStateChanged().listen([](const jukebox::event::SongStateChangedData&) {
        playback.clear(); manifests.clear();
    }).leak();
    jukebox::event::ManualSongAdded().listen([](const jukebox::event::ManualSongAddedData& event) {
        auto song = event.song(); auto meta = song->metadata();
        addedSongs[key(meta->gdID,meta->uniqueID)] = {meta->gdID,meta->startOffset,meta->uniqueID,meta->name,
            song->path().has_value()?string::pathToString(*song->path()):"",false};
        invalidate(meta->gdID);
    }).leak();
#ifndef GEODE_IS_MACOS
    MouseInputEvent().listen([](MouseInputData& event) {
        if (event.button != MouseInputData::Button::Right || event.action != MouseInputData::Action::Press) return ListenerResult::Propagate;
        rightClick(cocos::getMousePos());
        return ListenerResult::Propagate;
    }).leak();
#endif
    jukebox::event::SongDownloadFinished().listen([](const jukebox::event::SongDownloadFinishedData& event) {
        auto song = event.destination(); auto meta = song->metadata();
        requests.erase(key(meta->gdID,meta->uniqueID)); invalidate(meta->gdID);
        addedSongs.erase(key(meta->gdID,meta->uniqueID));
        if (auto c = choice(meta->gdID); c && c->uid == meta->uniqueID) {
            c->path = song->path().has_value()?string::pathToString(*song->path()):"";
            c->offset = meta->startOffset; c->name = meta->name; store(*c);
        }
        log::info("Jukebox song cached for OBS: {}",meta->name);
    }).leak();
    jukebox::event::SongDownloadFailed().listen([](const jukebox::event::SongDownloadFailedData& event) {
        invalidate(event.gdId());
        auto& r = requests[key(event.gdId(),std::string(event.uniqueId()))]; r = {false,std::string(event.error())};
        if (auto c = choice(event.gdId()); c && c->uid == event.uniqueId())
            Notification::create("OBS song download failed",NotificationIcon::Error)->show();
    }).leak();
    jukebox::event::NongDeleted().listen([](const jukebox::event::NongDeletedData& event) {
        invalidate(event.gdId());
        addedSongs.erase(key(event.gdId(),std::string(event.uniqueId())));
        if (auto c = choice(event.gdId()); c && c->uid == event.uniqueId()) clear(event.gdId());
    }).leak();
}
void rightClick(CCPoint point) {
    if (auto play = PlayLayer::get(); play && !play->m_isPaused) return;
    if (!cellLayoutMatches()) return;
    auto scene = CCDirector::get()->getRunningScene();
    auto list = findList(scene);
    if (!list || !unobscured(scene, list)) return;
    auto scroll = list->getChildByID("list");
    if (!scroll || !CCRect{{0, 0}, scroll->getContentSize()}.containsPoint(scroll->convertToNodeSpace(point))) return;
    for (auto cell : cells(list)) {
        if (!CCRect{{0, 0}, cell->getContentSize()}.containsPoint(cell->convertToNodeSpace(point))) continue;
        if (auto ui = CellAccess::ui(cell); ui && gameButtonShown(ui)) select(cell);
        break;
    }
}
}

class $modify(OBSJukeboxNode, cocos2d::CCNode) {
    void onEnter() {
        cocos2d::CCNode::onEnter();
        separate_song::jukebox_link::observeList(this);
    }
};
