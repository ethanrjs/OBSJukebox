#pragma once
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace separate_song {
template <class Claim, class Generate>
std::string claimLevelIdentity(std::string value, Claim claim, Generate generate) {
    while (value.empty() || !claim(value)) value = generate();
    return value;
}

inline std::vector<int> levelSongIDs(int initial, std::string_view declared) {
    std::vector<int> ids{initial};
    while (!declared.empty()) {
        auto end = declared.find(',');
        auto token = declared.substr(0, end);
        int id = 0;
        auto parsed = std::from_chars(token.data(), token.data() + token.size(), id);
        if (parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size() && id != 0 &&
            std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        if (end == std::string_view::npos) break;
        declared.remove_prefix(end + 1);
    }
    return ids;
}

struct MusicCandidate { int id; std::string path; };
inline std::optional<int> songForPath(std::string_view path, const std::vector<MusicCandidate>& candidates) {
    if (path.empty()) return std::nullopt;
    std::optional<int> result;
    for (const auto& candidate : candidates) {
        if (candidate.path != path) continue;
        if (result && *result != candidate.id) return std::nullopt;
        result = candidate.id;
    }
    return result;
}

}
