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

using namespace geode::prelude;
namespace separate_song::jukebox_link {
namespace {
using Clock = std::chrono::steady_clock;
struct Choice {
    int id = 0, offset = 0;
    std::string uid, name, path;
    bool original = false;
};
struct Request { bool pending = false; std::string error; };
std::unordered_map<std::string, Request> requests;
std::unordered_map<std::string, Choice> addedSongs;
struct CachedManifest { matjson::Value json; Clock::time_point read{}; };
std::unordered_map<int, CachedManifest> manifests;
struct CachedPlayback {
    std::optional<Choice> obs;
    Choice game;
    bool obsReady = false, gameReady = false;
    Clock::time_point read{};
};
std::unordered_map<int, CachedPlayback> playback;
void invalidate(int id) { playback.erase(id); manifests.erase(id); }

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
bool exists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    try {
        auto filePath = std::filesystem::u8path(path);
        return std::filesystem::is_regular_file(filePath, ec) &&
               std::filesystem::file_size(filePath, ec) > 0 && !ec;
    } catch (const std::filesystem::filesystem_error&) {
        return false;
    }
}
std::filesystem::path base() { return Loader::get()->getLoadedMod("fleym.nongd")->getSaveDir(); }
// CellAccess and UIAccess read Jukebox's protected fields using the vendored 3.8.0 headers.
// Geode lets the dependency update to any newer 3.x, so skip the row UI on other builds.
bool cellLayoutMatches() {
    static const bool matches = [] {
        auto dependency = Loader::get()->getLoadedMod("fleym.nongd");
        if (!dependency) return false;
        auto version = dependency->getVersion();
        bool ok = version.getMajor() == 3 && version.getMinor() == 8 && version.getPatch() == 0 && !version.getTag();
        if (!ok) log::warn("Jukebox {} is not 3.8.0; OBS checkboxes in Jukebox are disabled", version.toVString());
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
matjson::Value manifest(int id, bool force = false) {
    auto& cache = manifests[id]; auto now = Clock::now();
    if (force || now-cache.read > std::chrono::milliseconds(250)) {
        cache.json = file::readJson(base()/"manifest"/fmt::format("{}.json", id)).unwrapOr(matjson::Value());
        cache.read = now;
    }
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
    if (exists(path)) return path;
    auto name = song["filename"].asString().unwrapOr("");
    if (!name.empty()) return string::pathToString(base()/"nongs"/std::filesystem::u8path(name));
    return path;
}
std::string originalPath(int id) {
    if (id < 0) {
        auto name = LevelTools::getAudioFileName(-id-1);
        return CCFileUtils::get()->fullPathForFilename(name.c_str(), false);
    }
    auto name = string::pathToString(std::filesystem::u8path(MusicDownloadManager::sharedState()->pathForSongFolder(id).c_str()) /
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
    if (c.original && !exists(c.path)) c.path = originalPath(c.id);
    return c;
}
int songID(GJGameLevel* level) { return level->m_songID > 0 ? level->m_songID : -level->m_audioTrack-1; }
std::string normalizedMusicPath(const std::string& path) {
    if (path.empty()) return {};
    auto full = CCFileUtils::get()->fullPathForFilename(path.c_str(), false);
    try {
        auto value = string::pathToString(std::filesystem::u8path(full).lexically_normal());
        std::replace(value.begin(), value.end(), '\\', '/');
#ifdef GEODE_IS_WINDOWS
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::tolower(c); });
#endif
        return value;
    } catch (const std::filesystem::filesystem_error&) { return {}; }
}
std::string download(Choice c, bool retry) {
    c = resolved(c);
    if (exists(c.path)) return "";
    auto k = key(c.id, c.uid); auto& request = requests[k];
    if (request.pending) return "";
    if (!request.error.empty() && !retry) return request.error;
    request = {true, ""};
    log::info("OBS song cache miss: {} ({})", c.name, c.uid);
    if (c.original && c.id > 0) MusicDownloadManager::sharedState()->downloadSong(c.id);
    else if (c.original) request = {false, "Original song file is unavailable."};
    else jukebox::event::StartDownload().send({c.id, c.uid});
    return requests[k].error;
}
Choice active(int id) {
    auto m = manifest(id); auto uid = m["active"].asString().unwrapOr("");
    auto v = find(m, uid); bool original = uid.empty() || uid == m["default"]["unique_id"].asString().unwrapOr("");
    Choice c{id, static_cast<int>(v["offset"].asInt().unwrapOr(0)), uid,
        v["name"].asString().unwrapOr("In-game song"), pathFor(v), original};
    if (original && !exists(c.path)) c.path = originalPath(id);
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
    if (auto picked = choice(id)) {
        value.obs = resolved(*picked);
        value.obsReady = exists(value.obs->path);
    }
    return playback.insert_or_assign(id, std::move(value)).first->second;
}

CCNode* findList(CCNode* node) {
    if (!node || !node->isVisible()) return nullptr;
    if (node->getID() == "NongList") return node;
    if (auto children = node->getChildren()) {
        for (int i = children->count()-1; i >= 0; --i)
            if (auto found = findList(static_cast<CCNode*>(children->objectAtIndex(i)))) return found;
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
    void onSelect(CCObject*) { select(m_cell); }
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

void refreshUI() {
    if (auto play = PlayLayer::get(); play && !play->m_isPaused) return;
    if (!cellLayoutMatches()) return;
    auto scene = CCDirector::get()->getRunningScene(); if (!scene) return;
    auto list = findList(scene); if (!list) return;
    for (auto cell : cells(list)) paint(cell);
}
Readiness prepare(GJGameLevel* level, bool retry) {
    if (!Mod::get()->getSettingValue<bool>("enabled") || !level) return {};
    int id = songID(level);
    if (retry) invalidate(id);
    auto cached = forPlayback(id);
    if (retry) {
        if (cached.obs && cached.obs->original && !cached.obsReady) requests.erase(key(id, cached.obs->uid));
        if (cached.game.original && !cached.gameReady) requests.erase(key(id, cached.game.uid));
    }
    if (cached.obs && !cached.obsReady) { auto error = download(*cached.obs, retry); if (!error.empty()) return {false,error}; }
    if (!cached.gameReady) { auto error = download(cached.game, retry); if (!error.empty()) return {false,error}; }
    bool ready = (!cached.obs || cached.obsReady) && cached.gameReady;
    if (!ready) invalidate(id);
    return {ready, ""};
}
LevelSongs snapshotSongs(GJGameLevel* level) {
    return {songID(level), std::string(level->m_songIDs)};
}
void fill(Snapshot& state, const LevelSongs* songs, const MusicSource& source) {
    state.path.clear(); state.song.clear();
    if (!songs) return;
    int id = songs->initialID;
    if (source.channel) {
        if (source.path.empty()) return;
        std::vector<MusicCandidate> candidates;
        auto ids = levelSongIDs(id, songs->declaredIDs);
        for (int candidate : source.extraIDs)
            if (std::find(ids.begin(), ids.end(), candidate) == ids.end()) ids.push_back(candidate);
        for (int candidate : ids)
            candidates.push_back({candidate, normalizedMusicPath(forPlayback(candidate).game.path)});
        auto matched = songForPath(normalizedMusicPath(source.path), candidates);
        if (!matched) {
            state.song = "In-game song";
            auto full = CCFileUtils::get()->fullPathForFilename(source.path.c_str(), false);
            if (exists(full)) state.path = full;
            return;
        }
        id = *matched;
    }
    const auto& cached = forPlayback(id);
    const auto& obs = cached.obs ? *cached.obs : cached.game;
    state.song = obs.name;
    if (cached.obs ? cached.obsReady : cached.gameReady) state.path = obs.path;
    state.offset += (obs.offset-(state.musicPosition?cached.game.offset:0))/1000.0;
}
void initialize() {
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
void rightClick(CCPoint point){
    if(!cellLayoutMatches())return;
    auto scene=CCDirector::get()->getRunningScene();auto list=findList(scene);if(!list || !unobscured(scene,list))return;
    auto scroll=list->getChildByID("list");if(!scroll)return;
    if(!CCRect{{0,0},scroll->getContentSize()}.containsPoint(scroll->convertToNodeSpace(point)))return;
    for(auto cell:cells(list))if(CCRect{{0,0},cell->getContentSize()}.containsPoint(cell->convertToNodeSpace(point))){
        if(auto ui=CellAccess::ui(cell); ui && gameButtonShown(ui))select(cell);
        break;
    }
}
}
