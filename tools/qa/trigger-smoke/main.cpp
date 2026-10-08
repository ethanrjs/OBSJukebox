#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/loader/Dirs.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <cmath>

using namespace geode::prelude;
namespace {
bool enabled = false, ready = false, started = false, paused = false, resumed = false, reset = false;
std::filesystem::path root;
std::ofstream output;
std::chrono::steady_clock::time_point begin, last;
PauseLayer* findPause(CCNode* node) {
    if (!node) return nullptr;
    if (auto pause = typeinfo_cast<PauseLayer*>(node)) return pause;
    if (auto children = node->getChildren()) {
        for (auto child : CCArrayExt<CCNode*>(children)) if (auto pause = findPause(child)) return pause;
    }
    return nullptr;
}
bool inside(std::filesystem::path const& path) {
    auto p = std::filesystem::weakly_canonical(path).string();
    auto r = std::filesystem::weakly_canonical(root).string() + "/";
    return p.starts_with(r);
}
void requireInside(std::filesystem::path const& path) {
    if (!inside(path)) {
        if (output) output << "ISOLATION_FAILURE " << path.string() << std::endl;
        std::_Exit(70);
    }
}
void tone(std::filesystem::path const& path, double hz, unsigned seconds = 30) {
    requireInside(path);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    auto u16 = [&](unsigned v) { file.put(v); file.put(v >> 8); };
    auto u32 = [&](unsigned v) { u16(v); u16(v >> 16); };
    unsigned frames = 48000 * seconds, bytes = frames * 2;
    file.write("RIFF", 4); u32(bytes + 36); file.write("WAVEfmt ", 8);
    u32(16); u16(1); u16(1); u32(48000); u32(96000); u16(2); u16(16);
    file.write("data", 4); u32(bytes);
    for (unsigned i = 0; i < frames; ++i) u16(static_cast<int16_t>(6000 * std::sin(i * hz * 6.283185307179586 / 48000)));
}
void launchLevel() {
    auto songs = MusicDownloadManager::sharedState();
    for (auto [id, hz] : {std::pair{90000001, 330.0}, {90000002, 550.0}, {90000003, 770.0}})
        tone(songs->pathForSong(id), hz, id == 90000001 ? 1 : 30);
    auto replacement = root / "obs-long.wav";
    tone(replacement, 330);
    auto mod = Loader::get()->getLoadedMod("local.separate_song");
    if (!mod) { output << "PRODUCTION_MOD_MISSING" << std::endl; std::_Exit(74); }
    auto selections = matjson::Value::object();
    selections["90000001"] = matjson::makeObject({{"uid", "qa-long-song"}, {"name", "QA long OBS replacement"},
        {"path", replacement.string()}, {"offset", 0}, {"original", false}});
    mod->setSavedValue("obs-selections", selections);
    output << "EOF_FIXTURE game_seconds=1 obs_seconds=30" << std::endl;
    auto level = GJGameLevel::create();
    level->m_levelName = "Isolated OBS Jukebox trigger smoke";
    level->m_songID = 90000001;
    level->m_songIDs = "90000001,90000002,90000003";
    level->m_levelType = GJLevelType::Editor;
    level->m_levelString =
        "kA2,0,kA4,0,kA13,0,kA15,0,kA16,0,kA27,1,kA40,1,kA48,1;"
        "1,1934,2,150,3,300,36,1,392,90000002,406,1,432,1,408,1000,409,200;"
        "1,3605,2,400,3,300,36,1,432,1,418,1,406,0.2,10,1;"
        "1,1934,2,700,3,300,36,1,392,90000003,406,1,432,2,413,1;"
        "1,3602,2,850,3,300,36,1,392,1274,406,1;"
        "1,1934,2,1000,3,300,36,1,392,90000003,406,0.7,432,1;"
        "1,3605,2,1300,3,300,36,1,432,1,417,1,411,300;"
        "1,8,2,1700,3,15;1,1,2,10000,3,1000;";
    auto scene = CCScene::create();
    auto layer = PlayLayer::create(level, false, false);
    if (!layer) { output << "LEVEL_CREATE_FAILED" << std::endl; std::_Exit(71); }
    for (auto object : CCArrayExt<GameObject*>(layer->m_objects)) {
        if (auto trigger = typeinfo_cast<SongTriggerGameObject*>(object)) {
            output << "PARSED_SONG id=" << object->m_objectID << " x=" << object->getPositionX()
                   << " channel=" << trigger->m_songChannel << " binding_change_volume=" << trigger->m_changeVolume
                   << " binding_change_speed=" << trigger->m_changeSpeed
                   << " volume=" << trigger->m_volume << std::endl;
        }
    }
    scene->addChild(layer);
    CCDirector::sharedDirector()->replaceScene(scene);
    FMODAudioEngine::get()->setEffectsVolume(0);
    output << "LEVEL_CREATED quiet_game_sfx=0" << std::endl;
}
void tick() {
    if (!enabled || !ready) return;
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - begin).count();
    if (elapsed > 30) { output << "FINISHED" << std::endl; std::_Exit(0); }
    if (!started && elapsed > 2) { started = true; launchLevel(); return; }
    if (!started || std::chrono::duration<double>(now - last).count() < .05) return;
    last = now;
    auto layer = PlayLayer::get();
    if (!layer) return;
    if (!paused && elapsed > 5) { paused = true; layer->pauseGame(false); output << "PAUSE " << elapsed << std::endl; }
    if (!resumed && elapsed > 6) {
        resumed = true;
        auto pause = findPause(CCDirector::sharedDirector()->getRunningScene());
        output << "RESUME " << elapsed << " actual_pause_layer=" << (pause != nullptr) << std::endl;
        if (!pause) std::_Exit(72);
        pause->onResume(nullptr);
    }
    if (!reset && elapsed > 13) { reset = true; layer->resetLevel(); output << "RESTART " << elapsed << std::endl; }
    output << "SAMPLE t=" << elapsed << " level=" << layer->m_gameState.m_levelTime << " attempt=" << layer->m_attempts
           << " paused=" << layer->m_isPaused << " dead=" << (layer->m_player1 && layer->m_player1->m_isDead);
    for (int id = 0; id < 3; ++id) {
        auto engine = FMODAudioEngine::get();
        auto c = engine->getActiveMusicChannel(id);
        if (!c) continue;
        bool playing = false; unsigned ms = 0; float volume = 0;
        c->isPlaying(&playing); c->getPosition(&ms, FMOD_TIMEUNIT_MS); c->getVolume(&volume);
        auto m = engine->m_fmodMusic.find(id);
        output << " ch" << id << "={playing:" << playing << ",ms:" << ms << ",gain:" << volume;
        if (m != engine->m_fmodMusic.end()) output << ",file:" << m->second.m_filePath;
        output << "}";
    }
    output << std::endl;
}
}
$on_mod(Loaded) {
    auto env = std::getenv("OBS_JUKEBOX_QA_HOME");
    if (!env || std::string_view(env).empty()) return;
    root = env;
    requireInside(CCFileUtils::sharedFileUtils()->getWritablePath());
    requireInside(geode::dirs::getSaveDir());
    requireInside(Mod::get()->getSaveDir());
    output.open(root / "trigger-smoke.log");
    output << "ISOLATION_OK cocos=" << CCFileUtils::sharedFileUtils()->getWritablePath()
           << " geode=" << geode::dirs::getSaveDir().string() << std::endl;
    enabled = true;
}
class $modify(QaMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (enabled && !ready) { ready = true; begin = last = std::chrono::steady_clock::now(); }
        return true;
    }
};
class $modify(QaDirector, CCDirector) {
    void drawScene() { CCDirector::drawScene(); tick(); }
};
class $modify(QaGameManager, GameManager) {
    void windowed() {
        if (std::getenv("OBS_JUKEBOX_QA_HOME")) {
            setGameVariable("0025", true);
            m_toFullscreen = false;
        }
    }
    void firstLoad() { GameManager::firstLoad(); windowed(); }
    void dataLoaded(DS_Dictionary* dict) { GameManager::dataLoaded(dict); windowed(); }
};
