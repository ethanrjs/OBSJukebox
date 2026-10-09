#include "../../src/PlaybackIdentity.hpp"
#include "../../src/MusicSampling.hpp"
#include "../../src/GamePolicies.hpp"
#include "../../src/LinkPacket.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

using namespace separate_song;
static unsigned checks = 0;
static void check(bool condition, const char* description) {
    ++checks;
    if (!condition) { std::cerr << "FAIL: " << description << '\n'; std::exit(1); }
}

int main() {
    std::unordered_map<std::string, int> owners;
    int next = 0;
    auto claim = [&](std::string saved, int owner) {
        return claimLevelIdentity(std::move(saved), [&](const std::string& id) {
            auto [it, inserted] = owners.emplace(id, owner);
            return inserted || it->second == owner;
        }, [&] { return "uuid-" + std::to_string(++next); });
    };
    auto first = claim("", 1);
    auto second = claim("", 2);
    check(!first.empty() && first != second, "new local levels receive independent identities");
    std::unordered_map<std::string, double> offsets{{"local-id:" + first, 1.25}, {"local-id:" + second, -2.5}};
    check(claim(second, 2) == second && claim(first, 1) == first, "selection order preserves identities");
    owners.clear();
    check(claim(second, 20) == second && claim(first, 10) == first, "saved identities survive restart in reversed order");
    check(offsets["local-id:" + first] == 1.25 && offsets["local-id:" + second] == -2.5, "offsets stay attached to original identities");
    auto copy = claim("", 30);
    check(copy != first && !offsets.contains("local-id:" + copy), "copied level cannot overwrite original offset");
    auto importedCopy = claim(first, 40);
    check(importedCopy != first, "duplicate encoded identity is replaced for the second owner");
    check(claim(first, 10) == first, "duplicate protection retains original identity");
    auto oldLevel = claim("", 50);
    offsets["local:1"] = 99;
    offsets["local-name:Example"] = 88;
    check(!offsets.contains("local-id:" + oldLevel), "ambiguous legacy values are not assigned to an unrelated level");

    auto ids = levelSongIDs(-1, "123,456,123,-2,0,invalid,12oops,2147483648,");
    check(ids == std::vector<int>({-1, 123, 456, -2}), "candidate list supports built-in IDs and rejects malformed entries");
    check(levelSongIDs(123, "") == std::vector<int>({123}), "empty extra-song list preserves the initial ID");
    std::vector<MusicCandidate> songs{{123, "/songs/a.mp3"}, {456, "/nongs/song-b.mp3"}};
    check(songForPath("/songs/a.mp3", songs) == 123, "initial sound selects initial song");
    check(songForPath("/nongs/song-b.mp3", songs) == 456, "Song trigger selects B's replacement and offsets through B's ID");
    check(songForPath("/songs/a.mp3", songs) == 123, "restart can return from song B to A");
    check(!songForPath("/songs/unknown.mp3", songs), "unknown channel path cannot select initial song");
    check(!songForPath("", songs), "missing sampled identity cannot select initial song");
    songs.push_back({789, "/nongs/song-b.mp3"});
    check(!songForPath("/nongs/song-b.mp3", songs), "shared file cannot choose the wrong OBS replacement");
    songs.pop_back();
    songs.push_back({456, "/nongs/song-b.mp3"});
    check(songForPath("/nongs/song-b.mp3", songs) == 456, "duplicate candidates for the same ID are harmless");
    std::vector<MusicFade> fade{{100, 0.f}, {200, 1.f}, {300, 0.f}};
    check(musicFadeAt(fade, 100) == 0, "fade starts at specified parent DSP clock");
    check(musicFadeAt(fade, 150) == .5f, "fade-in interpolates linear amplitude");
    check(musicFadeAt(fade, 250) == .5f, "fade-out interpolates without using game master gain");
    check(musicFadeAt(fade, 500) == 0, "completed cached fade retains silence after FMOD removes points");
    check(musicFadeAt({}, 500) == 1, "explicitly reset envelope restores unit gain");
    check(finiteMusicGain(std::nanf("")) == 0, "invalid trigger volume cannot contaminate PCM");
    check(!musicPositionJump(2, 2.02, .02, 1), "normal source advance preserves voice epoch");
    check(musicPositionJump(9.99, .01, .02, 1), "loop wrap starts new decoder epoch");
    check(musicPositionJump(2, 5, .02, 1), "Song edit seek starts new decoder epoch");
    check(canContinueMusicEOF(9.99, 10, .02, 1, false, false), "natural level song EOF can extend replacement");
    check(!canContinueMusicEOF(9.99, 10, .02, 1, true, false), "looping song never becomes EOF continuation");
    check(!canContinueMusicEOF(9.99, 10, .02, 1, false, true), "explicit scheduled song end cannot extend replacement");
    check(!canContinueMusicEOF(8, 10, .02, 1, false, false), "early stopped song cannot extend replacement");
    check(!canContinueMusicEOF(9.99, 10, 2, 1, false, false), "stale sample cannot infer natural EOF");
    check(backgroundMusicGain(-1, .1f, 3) == 0, "checkpoint background fade starts silent independently of slider");
    check(std::abs(backgroundMusicGain(3, .1f, 3.05f) - .5f) < .001f, "checkpoint group fade reconstructs middle gain");
    check(backgroundMusicGain(0, .1f, 3) == 1, "completed background fade restores unit factor");
    std::vector<MusicFade> unordered{{300, .3f}, {100, .1f}, {100, .8f}, {200, std::nanf("")}};
    normalizeMusicFades(unordered);
    check(unordered.size() == 3 && unordered[0].clock == 100 && unordered[0].gain == .8f,
        "duplicate DSP clocks use the last gain after stable sorting");
    check(unordered[1].gain == 0 && musicFadeAt(unordered, 150) == .4f,
        "invalid DSP gain is sanitized before interpolation");
    std::array<SongFadePoint, 8> packet{{{20, .8f}, {10, .5f}, {20, 0.f}, {0, 1.f},
        {86400000000001ULL, 1.f}, {30, std::nanf("")}}};
    auto count = normalizePacketFades(std::span(packet), 6);
    check(count == 3 && packet[0].offsetNs == 10 && packet[1].offsetNs == 20 && packet[2].offsetNs == 30,
        "packet fade offsets are strictly increasing and inside protocol limits");
    check(packet[1].gain == 0 && packet[2].gain == 0,
        "scheduled stop wins duplicate timestamp and nonfinite gains are silent");
    std::cout << checks << " game identity checks passed\n";
}
