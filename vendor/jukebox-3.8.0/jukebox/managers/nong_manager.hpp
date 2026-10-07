#pragma once

#include <asp/fs.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

#include <Geode/Result.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/loader/Mod.hpp>
#include <Geode/utils/file.hpp>

#include <jukebox/nong/nong.hpp>

namespace jukebox {

class NongManager {
protected:
    Manifest m_manifest;
    bool m_initialized = false;

    NongManager() = default;

    void setupManifestPath() {
        const std::filesystem::path path = this->baseManifestPath();

        if (!asp::fs::exists(path)) {
            auto createDirRes = geode::utils::file::createDirectory(path);
            if (createDirRes.isErr()) {
                geode::log::error("Failed to create manifest path {}: {}", path, createDirRes.unwrapErr());
            }
        }
    }

    geode::Result<> saveNongs(std::optional<int> saveId = std::nullopt);
    geode::Result<std::unique_ptr<Nongs>> loadNongsFromPath(const std::filesystem::path& path);

    geode::Result<> migrateV2();

public:
    NongManager(const NongManager&) = delete;
    NongManager(NongManager&&) = delete;

    NongManager& operator=(const NongManager&) = delete;
    NongManager& operator=(NongManager&&) = delete;

    std::optional<Nongs*> m_currentlyPreparingNong = std::nullopt;

    bool init();

    bool initialized() const { return m_initialized; }

    std::filesystem::path baseManifestPath() {
        static std::filesystem::path path = geode::Mod::get()->getSaveDir() / "manifest";
        return path;
    }

    std::filesystem::path baseNongsPath() {
        static std::filesystem::path path = geode::Mod::get()->getSaveDir() / "nongs";
        return path;
    }

    [[nodiscard]] bool hasSongID(int id) const;

    geode::Result<Nongs*> initSongID(SongInfoObject* obj, int id, bool robtop);


    int adjustSongID(int id, bool robtop);


    void resolveSongInfoCallback(int id);


    [[nodiscard]] int getCurrentManifestVersion() const;


    [[nodiscard]] int getStoredIDCount() const;


    std::optional<Nongs*> getNongs(int songID);


    std::vector<std::string> getVerifiedNongsForLevel(int levelID, std::vector<int> songIDs);


    bool isNongVerifiedForLevelSong(int levelID, int songID, std::string_view uniqueID);


    bool isNongVerified(int levelID, std::vector<int> songIDs);


    std::string getFormattedSize(const std::filesystem::path& path);


    arc::Future<std::string> getMultiAssetSizes(std::string songs, std::string sfx, std::filesystem::path resourcesDir,
                                                std::filesystem::path songDir);


    void refetchDefault(int songID);


    geode::Result<> addNongs(Nongs&& nong);


    geode::Result<> setActiveSong(int gdSongID, std::string uniqueID);


    geode::Result<> deleteSong(int gdSongID, std::string uniqueID);


    geode::Result<> deleteSongAudio(int gdSongID, std::string uniqueID);


    geode::Result<> deleteAllSongs(int gdSongID);


    std::filesystem::path generateSongFilePath(const std::string& extension,
                                               std::optional<std::string> filename = std::nullopt);

    static NongManager& get() {
        static NongManager instance;
        return instance;
    }
};

}
