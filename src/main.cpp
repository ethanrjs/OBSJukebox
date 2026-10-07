#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelPage.hpp>
#include <Geode/modify/LevelSelectLayer.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include "Bridge.hpp"
#include "JukeboxLink.hpp"
#include "AudioTap.hpp"
#include "OffsetSetting.hpp"
#include "ObsVolume.hpp"
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>

using namespace geode::prelude;
using namespace separate_song;
static Snapshot state;
static Ref<GJGameLevel> playingLevel;
static std::string offsetLevel;
static bool loadingOffset = false;
static bool completedPlayback = false;
static bool completedPaused = false;
static double completedPosition = 0;
static std::chrono::steady_clock::time_point completedAt;

static void sampleCompletedMusic() {
    state.position = completedPosition + (completedPaused ? 0.0 :
        std::chrono::duration<double>(std::chrono::steady_clock::now() - completedAt).count() * state.rate);
}

static std::string levelOffsetKey(GJGameLevel* level) {
    if (!level) return "";
    int id = level->m_levelID.value();
    if (id > 0 && !level->m_isEditable)
        return fmt::format("{}:{}", level->m_levelType == GJLevelType::Main ? "official" : "online", id);
    if (level->m_levelIndex > 0) return fmt::format("local:{}", level->m_levelIndex);
    return fmt::format("local-name:{}", std::string(level->m_levelName));
}
static void selectOffsetLevel(GJGameLevel* level) {
    auto key = levelOffsetKey(level);
    if (key == offsetLevel && level) return;
    offsetLevel = std::move(key);
    auto values = Mod::get()->getSavedValue<matjson::Value>("obs-level-offsets", matjson::Value::object());
    double value = offsetLevel.empty() ? 0.0 : values[offsetLevel].asDouble().unwrapOr(0.0);
    loadingOffset = true;
    offset_setting::setValue(std::isfinite(value) ? value : 0.0);
    loadingOffset = false;
}

static void settings() {
    auto mod = Mod::get();
    state.enabled = mod->getSettingValue<bool>("enabled");
    audio_tap::enable(state.enabled);
    state.musicVolume = obs_volume::music();
    state.effectsVolume = obs_volume::effects();
    state.offset = playingLevel ? offset_setting::value() : 0.0;
    jukebox_link::fill(state, playingLevel.data());
}
static void publish(const std::string& status, bool playing) {
    settings();
    state.status = status;
    state.playing = playing;
    bridge().publish(state);
}

$on_mod(Loaded) {
    offset_setting::initialize();
    SettingChangedEventV3(Mod::get(), "offset").listen([](std::shared_ptr<SettingV3> setting) {
        auto offset = std::dynamic_pointer_cast<offset_setting::OffsetSetting>(setting);
        if (!offset || loadingOffset || offsetLevel.empty()) return;
        auto values = Mod::get()->getSavedValue<matjson::Value>("obs-level-offsets", matjson::Value::object());
        values[offsetLevel] = offset->getValue();
        Mod::get()->setSavedValue("obs-level-offsets", values);
    }).leak();
    selectOffsetLevel(nullptr);
    jukebox_link::initialize();
    settings();
    if (!bridge().start()) log::error("OBS Jukebox could not open its OBS link.");
    else log::info("OBS Jukebox native OBS link on loopback port {}", SONG_LINK_PORT);
    bridge().publish(state);
}

class $modify(SeparateSongPlay, PlayLayer) {
    struct Fields {
        std::chrono::steady_clock::time_point last{};
        bool started = false;
    };
    bool init(GJGameLevel* level, bool replay, bool dontCreateObjects) {
        completedPlayback = false;
        selectOffsetLevel(level);
        playingLevel = level;
        publish("Preparing", false);
        if (PlayLayer::init(level, replay, dontCreateObjects)) return true;
        playingLevel = nullptr;
        publish("Ready", false);
        return false;
    }
    void sample() {
        if (m_level && playingLevel.data() != m_level) {
            playingLevel = m_level;
            selectOffsetLevel(m_level);
        }
        if (completedPlayback) { sampleCompletedMusic(); return; }
        state.position = m_gameState.m_levelTime + (m_levelSettings ? m_levelSettings->m_songOffset : 0);
        state.musicPosition = false;
        state.rate = m_gameState.m_timeWarp > 0 ? std::clamp(double(m_gameState.m_timeWarp), .25, 4.0) : 1.0;
        auto channel = FMODAudioEngine::get()->getActiveMusicChannel(0);
        bool playing = false;
        if (channel && !(m_isPracticeMode && !m_practiceMusicSync) &&
            channel->isPlaying(&playing) == FMOD_OK && playing) {
            unsigned ms = 0;
            if (channel->getPosition(&ms, FMOD_TIMEUNIT_MS) == FMOD_OK) {
                state.position = ms / 1000.0;
                state.musicPosition = true;
            }
            float hz = 0, base = 0;
            FMOD::Sound* sound = nullptr;
            if (channel->getCurrentSound(&sound) == FMOD_OK && sound &&
                sound->getDefaults(&base, nullptr) == FMOD_OK && base > 0 &&
                channel->getFrequency(&hz) == FMOD_OK)
                state.rate = std::clamp(double(hz / base), .25, 4.0);
        }
        state.attempt = m_attempts;
        if (m_level) state.level = m_level->m_levelName;
    }
    void startMusic() {
        PlayLayer::startMusic();
        m_fields->started = true;
        ++state.epoch;
        sample();
        publish("Playing", m_started && !m_isPaused && m_player1 && !m_player1->m_isDead);
    }
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - m_fields->last).count() < .025) return;
        m_fields->last = now;
        sample();
        bool dead = m_player1 && m_player1->m_isDead;
        bool done = m_hasCompletedLevel;
        publish(done ? "Complete" : m_isPaused ? "Paused" : dead ? "Death" : "Playing",
                completedPlayback ? !completedPaused : (m_fields->started && m_started && !m_isPaused && !dead));
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        if (player->m_isDead) { sample(); publish("Death", false); }
    }
    void resetLevel() {
        completedPlayback = false;
        bool alreadyStarted = m_fields->started;
        m_fields->started = false;
        ++state.epoch;
        publish("Restarting", false);
        PlayLayer::resetLevel();
        sample();
        m_fields->started = m_fields->started || alreadyStarted || m_isPracticeMode;
        publish("Playing", m_fields->started && m_started && !m_isPaused && m_player1 && !m_player1->m_isDead);
    }
    void pauseGame(bool unfocused) {
        PlayLayer::pauseGame(unfocused);
        if (m_isPaused) {
            sample();
            if (completedPlayback) { completedPosition = state.position; completedPaused = true; }
            publish("Paused", false);
        }
    }
    void resume() {
        PlayLayer::resume();
        if (completedPlayback && completedPaused) {
            completedAt = std::chrono::steady_clock::now(); completedPaused = false;
        }
        sample();
        publish(completedPlayback ? "Complete" : "Playing", completedPlayback ||
            (m_fields->started && m_started && !m_isPaused && m_player1 && !m_player1->m_isDead));
    }
    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        sample();
        bool dead = m_player1 && m_player1->m_isDead;
        publish(completedPlayback ? "Complete" : m_isPaused ? "Paused" : dead ? "Death" : "Playing",
            completedPlayback ? !completedPaused : (m_fields->started && m_started && !m_isPaused && !dead));
    }
    void levelComplete() {
        sample();
        completedPosition = state.position;
        completedAt = std::chrono::steady_clock::now();
        PlayLayer::levelComplete();
        completedPaused = false;
        completedPlayback = true;
        publish("Complete", true);
    }
    void onExit() {
        if (playingLevel.data() == m_level) {
            completedPlayback = false;
            playingLevel = nullptr;
            publish("Ready", false);
        }
        PlayLayer::onExit();
    }
};

class SongDownloadWait : public CCNode {
    Ref<GJGameLevel> level;
    Ref<Notification> notice;
    geode::Function<void()> ready;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point lastPoll{};
public:
    static void begin(CCNode* parent, GJGameLevel* level, geode::Function<void()> ready) {
        if (parent->getChildByID("separate-song-download-wait")) return;
        auto wait = new SongDownloadWait();
        wait->init(); wait->autorelease();
        wait->setID("separate-song-download-wait");
        wait->level = level; wait->ready = std::move(ready);
        wait->start = std::chrono::steady_clock::now();
        wait->notice = Notification::create("Downloading game and OBS songs...", NotificationIcon::Loading, 0);
        wait->notice->show(); parent->addChild(wait); wait->scheduleUpdate();
    }
    void update(float) override {
        auto now = std::chrono::steady_clock::now();
        if (now-lastPoll < std::chrono::milliseconds(250)) return;
        lastPoll = now;
        auto result = jukebox_link::prepare(level.data());
        if (!result.ready && result.error.empty() &&
            std::chrono::steady_clock::now()-start < std::chrono::seconds(120)) return;
        Ref<SongDownloadWait> keep(this);
        auto callback = std::move(ready);
        notice->hide(); unscheduleUpdate(); removeFromParent();
        if (result.ready) callback();
        else FLAlertLayer::create("OBS Song", result.error.empty() ?
            "The download took too long. Retry from Jukebox, or choose a cached song." : result.error, "OK")->show();
    }
    void onExit() override {
        if (notice) notice->hide();
        CCNode::onExit();
    }
};

class $modify(SeparateSongInfo, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        selectOffsetLevel(level);
        return true;
    }
    void onPlay(CCObject* sender) {
        selectOffsetLevel(m_level);
        if (getChildByID("separate-song-download-wait")) return;
        auto result = jukebox_link::prepare(m_level, true);
        if (result.ready) LevelInfoLayer::onPlay(sender);
        else if (!result.error.empty()) FLAlertLayer::create("OBS Song", result.error, "OK")->show();
        else SongDownloadWait::begin(this, m_level, [this] { LevelInfoLayer::onPlay(nullptr); });
    }
};
class $modify(SeparateSongPage, LevelPage) {
    bool init(GJGameLevel* level) {
        if (!LevelPage::init(level)) return false;
        selectOffsetLevel(level);
        return true;
    }
    void onPlay(CCObject* sender) {
        selectOffsetLevel(m_level);
        if (getChildByID("separate-song-download-wait")) return;
        auto result = jukebox_link::prepare(m_level, true);
        if (result.ready) LevelPage::onPlay(sender);
        else if (!result.error.empty()) FLAlertLayer::create("OBS Song", result.error, "OK")->show();
        else SongDownloadWait::begin(this, m_level, [this] { LevelPage::onPlay(nullptr); });
    }
};
class $modify(SeparateSongEdit, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) return false;
        selectOffsetLevel(level);
        return true;
    }
};
class $modify(SeparateSongLevelSelect, LevelSelectLayer) {
    void selectCurrentOffset() {
        if (!m_scrollLayer || !m_scrollLayer->m_pages || !m_scrollLayer->m_pages->count()) return;
        auto page = static_cast<LevelPage*>(m_scrollLayer->getPage(m_scrollLayer->m_page));
        if (page) selectOffsetLevel(page->m_level);
    }
    bool init(int page) {
        if (!LevelSelectLayer::init(page)) return false;
        selectCurrentOffset();
        return true;
    }
    void scrollLayerMoved(CCPoint position) {
        LevelSelectLayer::scrollLayerMoved(position);
        selectCurrentOffset();
    }
};
class $modify(SeparateSongDirector, CCDirector) {
    void drawScene() {
        static auto last = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (now-last > std::chrono::milliseconds(100)) {
            last = now; audio_tap::install(); jukebox_link::refreshUI();
            if (completedPlayback) { sampleCompletedMusic(); settings(); bridge().publish(state); }
            else if (!state.playing) { settings(); bridge().publish(state); }
        }
        CCDirector::drawScene();
    }
};

class $modify(SeparateSongMenu, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        publish("Ready", false);
        return true;
    }
};
