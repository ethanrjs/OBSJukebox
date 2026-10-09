#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
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
#include "LevelIdentity.hpp"
#include "PlaybackIdentity.hpp"
#include "MusicSampling.hpp"
#include "GamePolicies.hpp"
#include "MonotonicClock.hpp"
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cmath>
#include <optional>

using namespace geode::prelude;
using namespace separate_song;
static Snapshot state;
static std::optional<jukebox_link::LevelSongs> playingSongs;
static PlayLayer* activePlayLayer = nullptr;
static std::string offsetLevel;
static bool loadingOffset = false;
static bool enabled = true;
struct MusicVoice {
    Snapshot snapshot;
    jukebox_link::MusicSource source;
    FMOD::Channel* channel = nullptr;
    FMOD::Sound* sound = nullptr;
    double duration = 0;
    bool loop = false, scheduledStop = false, continuing = false, explicitlyStopped = false, dontReset = false;
    int engineChannelID = 0;
    float lastFade = 1.f;
    std::vector<MusicFade> fadePoints;
};
static std::map<int, MusicVoice> voices;
static unsigned nextEpoch = 1;
static void cancelVoice(int id, bool stopped = true) {
    if (auto it = voices.find(id); it != voices.end()) {
        it->second.continuing = false;
        it->second.duration = 0;
        it->second.explicitlyStopped = stopped;
        it->second.snapshot.epoch = ++nextEpoch;
    }
}
static void clearVoiceFade(FMOD::Channel* channel) {
    for (auto& [id, voice] : voices) if (voice.channel == channel) {
        voice.fadePoints.clear(); voice.lastFade = 1.f;
    }
}
static bool channelPaused(FMOD::Channel* channel) {
    bool paused = false;
    if (channel->getPaused(&paused) != FMOD_OK || paused) return true;
    FMOD::ChannelGroup* group = nullptr;
    channel->getChannelGroup(&group);
    for (int depth = 0; group && depth < 16; ++depth) {
        if (group->getPaused(&paused) != FMOD_OK || paused) return true;
        FMOD::ChannelGroup* parent = nullptr;
        if (group->getParentGroup(&parent) != FMOD_OK || parent == group) break;
        group = parent;
    }
    return false;
}
static double channelRate(FMOD::Channel* channel, FMOD::Sound* sound) {
    float base = 0.f, frequency = 0.f, pitch = 1.f;
    sound->getDefaults(&base, nullptr);
    channel->getFrequency(&frequency);
    channel->getPitch(&pitch);
    double rate = base > 0 && frequency > 0 ? frequency / base : 1.;
    rate *= pitch;
    FMOD::ChannelGroup* group = nullptr;
    channel->getChannelGroup(&group);
    for (int depth = 0; group && depth < 16; ++depth) {
        pitch = 1.f; group->getPitch(&pitch); rate *= pitch;
        FMOD::ChannelGroup* parent = nullptr;
        if (group->getParentGroup(&parent) != FMOD_OK || parent == group) break;
        group = parent;
    }
    return std::isfinite(rate) ? std::clamp(rate, .25, 4.) : 1.;
}
// A channel's position in seconds and the wall time it is true at. FMOD advances the position
// once per mixer block (~21 ms), so dating it with the DSP clock of the effects group, which the
// effects tap reads, puts it on the tap's timeline. The main thread's own clock would be up to a
// block late. The music group's clock would not do: FMOD stops a group's clock while it is
// paused, and GD pauses the effects group alone when it goes to the background, so the two
// clocks drift apart. PCM units are exact where milliseconds truncate.
static std::optional<std::pair<double, uint64_t>> playbackPosition(FMOD::Channel* channel, FMOD::Sound* sound,
                                                                    FMOD::ChannelGroup* effects) {
    float soundRate = 0.f;
    sound->getDefaults(&soundRate, nullptr);
    auto unit = soundRate > 0 ? FMOD_TIMEUNIT_PCM : FMOD_TIMEUNIT_MS;
    unsigned position = 0;
    unsigned long long clock = 0, after = 0;
    for (int attempt = 0; attempt < 2; ++attempt) { // retry once if a mix lands between the reads
        if (effects) effects->getDSPClock(&clock, nullptr);
        if (channel->getPosition(&position, unit) != FMOD_OK) return std::nullopt;
        if (effects) effects->getDSPClock(&after, nullptr);
        if (after == clock) break;
    }
    double seconds = soundRate > 0 ? position / double(soundRate) : position / 1000.0;
    auto wall = effects ? audio_tap::wallAtDspClock(after) : std::nullopt;
    return std::pair{seconds, wall.value_or(monotonicNowNs())};
}
static void sampleEnvelope(MusicVoice& voice, FMOD::Channel* channel, unsigned sampleRate) {
    auto& out = voice.snapshot;
    float volume = 1.f;
    channel->getVolume(&volume);
    unsigned long long clock = 0;
    channel->getDSPClock(nullptr, &clock);
    unsigned count = 0;
    if (channel->getFadePoints(&count, nullptr, nullptr) == FMOD_OK && count) {
        count = std::min(count, 128u);
        std::vector<unsigned long long> clocks(count);
        std::vector<float> gains(count);
        if (channel->getFadePoints(&count, clocks.data(), gains.data()) == FMOD_OK) {
            voice.fadePoints.clear();
            count = std::min(count, static_cast<unsigned>(clocks.size()));
            for (unsigned i = 0; i < count; ++i) voice.fadePoints.push_back({clocks[i], gains[i]});
            normalizeMusicFades(voice.fadePoints);
        }
    }
    if (!voice.fadePoints.empty()) voice.lastFade = musicFadeAt(voice.fadePoints, clock);
    out.triggerGain = finiteMusicGain(finiteMusicGain(volume) * voice.lastFade);
    out.fadeCount = 0;
    if (!sampleRate || !out.playing) return;
    unsigned long long start = 0, end = 0;
    channel->getDelay(&start, &end, nullptr);
    auto limit = end > clock ? out.fades.size() - 2 : out.fades.size();
    for (const auto& point : voice.fadePoints) {
        if (point.clock <= clock || (end && point.clock >= end)) continue;
        if (out.fadeCount == limit) break;
        out.fades[out.fadeCount++] = {(point.clock - clock) * 1000000000ull / sampleRate,
            finiteMusicGain(finiteMusicGain(volume) * finiteMusicGain(point.gain))};
    }
    if (end > clock) {
        auto offset = (end - clock) * 1000000000ull / sampleRate;
        float beforeEnd = finiteMusicGain(finiteMusicGain(volume) * musicFadeAt(voice.fadePoints, end));
        if (offset > 1) out.fades[out.fadeCount++] = {offset - 1, beforeEnd};
        out.fades[out.fadeCount++] = {offset, 0.f};
    }
    out.fadeCount = normalizePacketFades(std::span(out.fades), out.fadeCount);
}

static std::string levelOffsetKey(GJGameLevel* level) {
    if (!level) return "";
    int id = level->m_levelID.value();
    if (id > 0 && !level->m_isEditable)
        return fmt::format("{}:{}", level->m_levelType == GJLevelType::Main ? "official" : "online", id);
    return "local-id:" + localLevelIdentity(level);
}
static void selectOffsetLevel(GJGameLevel* level) {
    auto key = levelOffsetKey(level);
    if (key == offsetLevel && level) return;
    offsetLevel = std::move(key);
    offset_setting::selectLevel(level ? (std::string(level->m_levelName).empty() ? "Unnamed level" : std::string(level->m_levelName)) : "");
    auto values = Mod::get()->getSavedValue<matjson::Value>("obs-level-offsets", matjson::Value::object());
    double value = offsetLevel.empty() ? 0.0 : values[offsetLevel].asDouble().unwrapOr(0.0);
    loadingOffset = true;
    offset_setting::setValue(std::isfinite(value) ? value : 0.0);
    loadingOffset = false;
}

static void settings(Snapshot& snapshot) {
    snapshot.enabled = enabled;
    audio_tap::enable(snapshot.enabled);
    snapshot.musicVolume = obs_volume::music();
    snapshot.effectsVolume = obs_volume::effects();
    snapshot.offset = playingSongs ? offset_setting::value() : 0.0;
}
static void publish(const std::string& status, bool playing) {
    settings(state);
    state.channelID = -1;
    state.timestamp = monotonicNowNs();
    state.status = status;
    state.playing = playing;
    bridge().publish(state);
}
static void stopVoices() {
    for (auto& [id, voice] : voices) {
        // Live positions are dated by the mixer and can be up to a block ahead of now. A stop must
        // not sort before them, or OBS would play on until they go stale.
        voice.snapshot.timestamp = std::max(voice.snapshot.timestamp, monotonicNowNs());
        voice.snapshot.playing = false;
        voice.snapshot.path.clear();
        voice.snapshot.epoch = ++nextEpoch;
        bridge().publish(voice.snapshot);
    }
    voices.clear();
}
// Samples one live GD music channel into its voice and publishes it. With a PlayLayer, the level's
// OBS choice can replace the song. With null (menus, shop, editor), OBS plays GD's own file.
static bool sampleChannel(FMODAudioEngine* engine, int id, FMODMusic& music, PlayLayer* play) {
    if (id < 0 || id >= 64) return false;
    auto channel = engine->getActiveMusicChannel(id);
    bool live = false;
    FMOD::Sound* sound = nullptr;
    if (!channel || channel->isPlaying(&live) != FMOD_OK || !live ||
        channel->getCurrentSound(&sound) != FMOD_OK || sound != music.m_sound) return false;
    auto sampled = playbackPosition(channel, sound, engine->m_globalChannel);
    if (!sampled) return false;
    auto [position, sampleTime] = *sampled;
    auto& voice = voices[id];
    auto& out = voice.snapshot;
    const auto previousTimestamp = out.timestamp;
    const auto previousPosition = out.position;
    const auto previousRate = out.rate;
    const auto previousPlaying = out.playing;
    const auto previousPath = out.path;
    bool newVoice = voice.channel != channel || voice.sound != sound ||
        voice.source.path != std::string(music.m_filePath) || voice.engineChannelID != music.m_channelID || voice.continuing;
    if (newVoice) {
        voice.fadePoints.clear(); voice.lastFade = 1.f; voice.explicitlyStopped = false;
    }
    voice.engineChannelID = music.m_channelID; voice.dontReset = music.m_dontReset;
    voice.channel = channel; voice.sound = sound; voice.continuing = false;
    voice.source = {true, std::string(music.m_filePath)};
    if (play) {
        if (auto entry = play->m_gameState.m_songChannelStates.find(id);
            entry != play->m_gameState.m_songChannelStates.end()) {
            for (auto trigger : {entry->second.m_songTriggerGameObject1, entry->second.m_songTriggerGameObject2})
                if (trigger && trigger->m_soundID > 0) voice.source.extraIDs.push_back(trigger->m_soundID);
        }
    }
    out.channelID = id; out.timestamp = sampleTime; out.musicPosition = true;
    out.position = position;
    out.playing = !channelPaused(channel);
    out.rate = channelRate(channel, sound);
    double elapsed = previousTimestamp && sampleTime >= previousTimestamp ? (sampleTime - previousTimestamp) / 1e9 : 0.;
    if (newVoice || !previousTimestamp || musicPositionJump(previousPosition, out.position,
            previousPlaying ? elapsed : 0., previousRate)) out.epoch = ++nextEpoch;
    unsigned duration = 0;
    sound->getLength(&duration, FMOD_TIMEUNIT_MS);
    voice.duration = duration / 1000.0;
    int loops = 0;
    FMOD_MODE mode = FMOD_DEFAULT;
    const bool loopMode = channel->getMode(&mode) == FMOD_OK &&
        (mode & (FMOD_LOOP_NORMAL | FMOD_LOOP_BIDI));
    channel->getLoopCount(&loops);
    voice.loop = loopMode && loops != 0;
    out.looping = false; out.loopStart = 0; out.loopEnd = 0;
    unsigned loopStart = 0, loopEnd = 0;
    float baseFrequency = 0.f;
    if (loopMode && (mode & FMOD_LOOP_NORMAL) && loops == -1 && sound->getDefaults(&baseFrequency, nullptr) == FMOD_OK && baseFrequency > 0 &&
        channel->getLoopPoints(&loopStart, FMOD_TIMEUNIT_PCM, &loopEnd, FMOD_TIMEUNIT_PCM) == FMOD_OK &&
        loopEnd >= loopStart) {
        out.looping = true;
        out.loopStart = loopStart / double(baseFrequency);
        out.loopEnd = (double(loopEnd) + 1.) / double(baseFrequency);
    }
    unsigned long long start = 0, end = 0, parentClock = 0;
    bool stops = false;
    channel->getDelay(&start, &end, &stops);
    channel->getDSPClock(nullptr, &parentClock);
    if ((start && parentClock < start) || (end && parentClock >= end)) out.playing = false;
    voice.scheduledStop = stops && end != 0;
    sampleEnvelope(voice, channel, engine->m_sampleRate);
    auto groupGain = backgroundMusicGain(engine->m_musicFadeStart,
        engine->m_backgroundMusicFade, engine->m_audioState.m_elapsed);
    out.triggerGain *= groupGain;
    for (unsigned i = 0; i < out.fadeCount; ++i) out.fades[i].gain *= groupGain;
    settings(out);
    out.status = out.playing ? "Playing" : "Paused";
    out.level = state.level; out.attempt = state.attempt;
    if (play) jukebox_link::fill(out, playingSongs ? &*playingSongs : nullptr, voice.source);
    else {
        if (newVoice || out.path.empty())
            out.path = CCFileUtils::get()->fullPathForFilename(music.m_filePath.c_str(), false);
        out.song = "GD music";
    }
    if (previousPath != out.path && !newVoice) out.epoch = ++nextEpoch;
    if (id == 0 || state.song.empty()) state.song = out.song;
    bridge().publish(out);
    return true;
}
// Publishes a stop for a voice whose channel is gone and forgets it.
static std::map<int, MusicVoice>::iterator endVoice(std::map<int, MusicVoice>::iterator it, uint64_t now) {
    auto& out = it->second.snapshot;
    out.timestamp = std::max(now, out.timestamp); out.playing = false; out.path.clear(); out.epoch = ++nextEpoch;
    bridge().publish(out);
    return voices.erase(it);
}
static void sampleMusic(PlayLayer* play) {
    if (!play || play != activePlayLayer) return;
    if (play->m_level) {
        state.level = play->m_level->m_levelName;
    }
    state.attempt = play->m_attempts;
    const auto now = monotonicNowNs();
    state.song.clear();
    auto engine = FMODAudioEngine::get();
    std::set<int> seen;
    for (auto& [id, music] : engine->m_fmodMusic)
        if (sampleChannel(engine, id, music, play)) seen.insert(id);
    for (auto it = voices.begin(); it != voices.end();) {
        auto& [id, voice] = *it;
        if (seen.contains(id)) { ++it; continue; }
        auto& out = voice.snapshot;
        double elapsed = now >= out.timestamp ? (now - out.timestamp) / 1e9 : 0.;
        bool continueEOF = id == 0 && (voice.continuing || (out.playing &&
            canContinueMusicEOF(out.position, voice.duration, elapsed, out.rate,
                voice.loop, voice.scheduledStop || voice.explicitlyStopped)));
        if (continueEOF) {
            if (out.playing) out.position += elapsed * out.rate;
            voice.continuing = true;
            out.timestamp = std::max(now, out.timestamp);
            bool paused = false;
            if (engine->m_backgroundMusicChannel) engine->m_backgroundMusicChannel->getPaused(&paused);
            bool deathPause = play->m_player1 && play->m_player1->m_isDead &&
                !play->m_gameState.m_audioOnDeath && !voice.dontReset &&
                !(play->m_isPracticeMode && !play->m_practiceMusicSync);
            out.playing = !paused && !play->m_isPaused && !deathPause;
            if (!paused && out.fadeCount) {
                std::vector<MusicFade> points{{0, out.triggerGain}};
                for (unsigned i = 0; i < out.fadeCount; ++i)
                    points.push_back({out.fades[i].offsetNs, out.fades[i].gain});
                auto passed = uint64_t(elapsed * 1e9);
                out.triggerGain = musicFadeAt(points, passed);
                unsigned count = 0;
                for (unsigned i = 0; i < out.fadeCount; ++i) {
                    if (out.fades[i].offsetNs <= passed) continue;
                    out.fades[count++] = {out.fades[i].offsetNs - passed, out.fades[i].gain};
                }
                out.fadeCount = count;
            }
            settings(out);
            jukebox_link::fill(out, playingSongs ? &*playingSongs : nullptr, voice.source);
            if (id == 0 || state.song.empty()) state.song = out.song;
            bridge().publish(out); ++it;
        } else it = endVoice(it, now);
    }
    bool dead = play->m_player1 && play->m_player1->m_isDead;
    publish(play->m_hasCompletedLevel ? "Complete" : play->m_isPaused ? "Paused" : dead ? "Death" : "Playing",
        !play->m_isPaused);
}
// Stops the level's songs in OBS and drops its name and attempt count from the status.
static void leaveLevel() {
    activePlayLayer = nullptr; playingSongs.reset(); stopVoices();
    state.level.clear(); state.song.clear(); state.attempt = 0;
    publish("Ready", false);
}
// Outside a level OBS mirrors whatever GD plays: the menu song, shop music, editor playtests.
static void sampleMusicOutsideLevels() {
    const auto now = monotonicNowNs();
    state.song.clear();
    auto engine = FMODAudioEngine::get();
    std::set<int> seen;
    for (auto& [id, music] : engine->m_fmodMusic)
        if (sampleChannel(engine, id, music, nullptr)) seen.insert(id);
    for (auto it = voices.begin(); it != voices.end();)
        it = seen.contains(it->first) ? std::next(it) : endVoice(it, now);
    publish("Ready", false);
}

$on_mod(Loaded) {
    offset_setting::initialize();
    enabled = Mod::get()->getSettingValue<bool>("enabled");
    listenForSettingChanges<bool>("enabled", [](bool value) { enabled = value; });
    SettingChangedEventV3(Mod::get(), "offset").listen([](std::shared_ptr<SettingV3> setting) {
        auto offset = std::dynamic_pointer_cast<offset_setting::OffsetSetting>(setting);
        if (!offset || loadingOffset || offsetLevel.empty()) return;
        auto values = Mod::get()->getSavedValue<matjson::Value>("obs-level-offsets", matjson::Value::object());
        values[offsetLevel] = offset->getValue();
        Mod::get()->setSavedValue("obs-level-offsets", values);
    }).leak();
    selectOffsetLevel(nullptr);
    jukebox_link::initialize();
    settings(state);
    if (!bridge().start()) log::error("OBS Jukebox could not open its OBS link.");
    else log::info("OBS Jukebox native OBS link on loopback port {}", SONG_LINK_PORT);
    bridge().publish(state);
}

class $modify(SeparateSongMusicEvents, FMODAudioEngine) {
    void stopAndRemoveMusic(int id) {
        cancelVoice(id);
        FMODAudioEngine::stopAndRemoveMusic(id);
    }
    void stopAllMusic(bool clear) {
        for (auto& [id, voice] : voices) {
            auto entry = m_fmodMusic.find(id);
            if (clear || entry == m_fmodMusic.end() || !entry->second.m_dontReset) cancelVoice(id);
        }
        FMODAudioEngine::stopAllMusic(clear);
    }
    void clearAllAudio() {
        for (auto& [id, voice] : voices) cancelVoice(id);
        FMODAudioEngine::clearAllAudio();
    }
    void stopChannel(FMOD::Channel* channel, bool loop, float delay) {
        for (auto& [id, voice] : voices) if (voice.channel == channel) cancelVoice(id);
        FMODAudioEngine::stopChannel(channel, loop, delay);
    }
    void setMusicTimeMS(unsigned time, bool dontWait, int id) {
        cancelVoice(id, false);
        FMODAudioEngine::setMusicTimeMS(time, dontWait, id);
    }
    float stopAndGetFade(FMOD::Channel* channel) {
        auto result = FMODAudioEngine::stopAndGetFade(channel);
        clearVoiceFade(channel);
        return result;
    }
};
class $modify(SeparateSongPlay, PlayLayer) {
    struct Fields {
        GJGameLevel* observedLevel = nullptr;
        uint64_t lastSample = 0;
        std::optional<MusicVoice> suspendedContinuation;
        bool quit = false;
    };
    void activateLink() {
        if (m_fields->quit) return; // a level that has quit never takes the link back from GD music
        activePlayLayer = this;
        if (m_level) playingSongs = jukebox_link::snapshotSongs(m_level);
        else playingSongs.reset();
        selectOffsetLevel(m_level);
        if (m_fields->suspendedContinuation) {
            auto& voice = *m_fields->suspendedContinuation;
            voice.snapshot.timestamp = std::max(voice.snapshot.timestamp, monotonicNowNs());
            voice.snapshot.playing = false;
            voice.snapshot.epoch = ++nextEpoch;
            voices[0] = std::move(voice);
            m_fields->suspendedContinuation.reset();
        }
    }
    bool init(GJGameLevel* level, bool replay, bool dontCreateObjects) {
        stopVoices();
        jukebox_link::prepare(level);
        selectOffsetLevel(level);
        activePlayLayer = this;
        m_fields->observedLevel = level;
        if (level) playingSongs = jukebox_link::snapshotSongs(level);
        else playingSongs.reset();
        publish("Preparing", false);
        if (PlayLayer::init(level, replay, dontCreateObjects)) return true;
        if (activePlayLayer == this) leaveLevel();
        return false;
    }
    void startMusic() { activateLink(); PlayLayer::startMusic(); sampleMusic(this); }
    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        // A switcher can reuse the current layer without a scene-enter or
        // resume callback. Reclaim only the live current layer, never one
        // retained by an outgoing scene.
        if (activePlayLayer != this && PlayLayer::get() == this && isRunning())
            activateLink();
        if (m_fields->observedLevel != m_level) {
            m_fields->observedLevel = m_level;
            if (m_level) playingSongs = jukebox_link::snapshotSongs(m_level);
            else playingSongs.reset();
            selectOffsetLevel(m_level);
        }
        auto now = monotonicNowNs();
        if (now - m_fields->lastSample >= 10000000) {
            m_fields->lastSample = now; sampleMusic(this);
        }
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        sampleMusic(this);
    }
    void resetLevel() {
        m_fields->suspendedContinuation.reset();
        activateLink();
        // A paused restart can reuse channel/sound pointers. Rebuild voice
        // state for the new attempt, including its envelope and epoch.
        stopVoices();
        PlayLayer::resetLevel(); sampleMusic(this);
    }
    void pauseGame(bool unfocused) { PlayLayer::pauseGame(unfocused); sampleMusic(this); }
    void resume() { activateLink(); PlayLayer::resume(); sampleMusic(this); }
    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish(); activateLink(); sampleMusic(this);
    }
    void levelComplete() { PlayLayer::levelComplete(); sampleMusic(this); }
    // Quitting stops the level's songs and starts the menu song, but this PlayLayer lives on until
    // GD has torn the level down. GD's music is mirrored from here instead of after that.
    void onQuit() {
        PlayLayer::onQuit();
        m_fields->quit = true;
        if (activePlayLayer == this) leaveLevel();
    }
    void onExit() {
        if (activePlayLayer == this) {
            if (auto found = voices.find(0); found != voices.end() && found->second.continuing)
                m_fields->suspendedContinuation = found->second;
            leaveLevel();
        }
        PlayLayer::onExit();
    }
};

static void prepareOBS(GJGameLevel* level) {
    auto result = jukebox_link::prepare(level, true);
    if (!result.ready && !result.error.empty())
        Notification::create("OBS song unavailable; following game audio", NotificationIcon::Info)->show();
}

class $modify(SeparateSongInfo, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) return false;
        selectOffsetLevel(level);
        return true;
    }
    void onPlay(CCObject* sender) {
        selectOffsetLevel(m_level);
        prepareOBS(m_level);
        LevelInfoLayer::onPlay(sender);
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
        prepareOBS(m_level);
        LevelPage::onPlay(sender);
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
        static auto last = std::chrono::steady_clock::time_point{}, lastMusic = last;
        auto now = std::chrono::steady_clock::now();
        auto play = PlayLayer::get();
        // A level samples its own music from postUpdate. Any other PlayLayer that still exists is
        // only mirrored once it has quit, so a played level's song never reaches OBS as GD music
        // in place of its OBS choice.
        bool outsideLevel = !play || static_cast<SeparateSongPlay*>(play)->m_fields->quit;
        if (now-last > std::chrono::milliseconds(100)) {
            last = now; audio_tap::install(); jukebox_link::refreshUI();
            if (play && play == activePlayLayer) sampleMusic(play);
            else if (!outsideLevel) publish("Ready", false);
        }
        if (outsideLevel && now-lastMusic >= std::chrono::milliseconds(10)) {
            lastMusic = now; sampleMusicOutsideLevels();
        }
        CCDirector::drawScene();
    }
};

class $modify(SeparateSongMenu, MenuLayer) {
    void onEnter() {
        MenuLayer::onEnter();
        selectOffsetLevel(nullptr);
    }
    bool init() {
        if (!MenuLayer::init()) return false;
        selectOffsetLevel(nullptr);
        publish("Ready", false);
        return true;
    }
};
