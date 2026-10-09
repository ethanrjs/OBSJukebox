#include "LevelIdentity.hpp"
#include "PlaybackIdentity.hpp"
#include <Geode/Geode.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/utils/random.hpp>
#include <unordered_map>

using namespace geode::prelude;
namespace {
constexpr auto identityKey = "local.separate_song/level-identity";
std::unordered_map<std::string, WeakRef<GJGameLevel>> owners;
}

class $modify(OBSLevelIdentity, GJGameLevel) {
    struct Fields { std::string identity; };
    std::string identity() {
        // Only inspect the claimed key; sweeping every loaded level was quadratic.
        // Periodic cleanup is amortized across additions, not every save.
        static size_t nextSweep = 1024;
        if (owners.size() >= nextSweep) {
            std::erase_if(owners, [](const auto& entry) { return !entry.second.lock(); });
            nextSweep = std::max(size_t(1024), owners.size() * 2);
        }
        m_fields->identity = separate_song::claimLevelIdentity(m_fields->identity,
            [this](const std::string& value) {
                auto found = owners.find(value);
                if (found != owners.end()) {
                    auto owner = found->second.lock();
                    if (owner && owner.data() != this) return false;
                }
                owners.insert_or_assign(value, WeakRef<GJGameLevel>(this));
                return true;
            }, [] { return geode::utils::random::generateUUID(); });
        return m_fields->identity;
    }
    void dataLoaded(DS_Dictionary* dict) {
        GJGameLevel::dataLoaded(dict);
        m_fields->identity = dict->getStringForKey(identityKey);
        if (m_isEditable || m_levelID.value() <= 0) identity();
    }
    void encodeWithCoder(DS_Dictionary* dict) {
        GJGameLevel::encodeWithCoder(dict);
        if (m_isEditable || m_levelID.value() <= 0) dict->setStringForKey(identityKey, identity());
    }
};

std::string separate_song::localLevelIdentity(GJGameLevel* level) {
    return static_cast<OBSLevelIdentity*>(level)->identity();
}
