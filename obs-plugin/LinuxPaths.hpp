#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class LinuxPaths {
    using Path = std::filesystem::path;
    std::vector<Path> prefixes;
    Path game;
    static std::string env(const char* key) {
        auto value=std::getenv(key);return value?value:"";
    }
    static std::string lower(std::string text) {
        for(auto& c:text)c=char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }
    static Path existing(Path root,const Path& suffix) {
        std::error_code ec;
        for(const auto& part:suffix){
            if(part=="." || part.empty())continue;
            if(part=="..")return {};
            auto direct=root/part;
            if(std::filesystem::exists(direct,ec)){root=direct;continue;}
            Path match;
            auto wanted=lower(part.string());
            for(std::filesystem::directory_iterator it(root,ec),end;!ec && it!=end;it.increment(ec)){
                if(lower(it->path().filename().string())!=wanted)continue;
                if(!match.empty())return {};
                match=it->path();
            }
            if(match.empty())return {};
            root=match;
        }
        return std::filesystem::is_regular_file(root,ec)?root:Path{};
    }
public:
    LinuxPaths() {
        auto home=env("HOME"),config=env("XDG_CONFIG_HOME");
        if(config.empty() && !home.empty())config=home+"/.config";
        std::string prefix,gamePath;
        if(!config.empty()){
            std::ifstream file(Path(config)/"obs-jukebox/paths");
            std::getline(file,prefix);std::getline(file,gamePath);
            if(!prefix.empty() && prefix.back()=='\r')prefix.pop_back();
            if(!gamePath.empty() && gamePath.back()=='\r')gamePath.pop_back();
        }
        auto explicitPrefix=env("OBS_JUKEBOX_WINE_PREFIX");
        if(!explicitPrefix.empty())prefix=explicitPrefix;
        auto explicitGame=env("OBS_JUKEBOX_GAME_DIRECTORY");
        if(!explicitGame.empty())gamePath=explicitGame;
        if(Path(gamePath).is_absolute())game=gamePath;
        auto add=[&](Path path){if(path.is_absolute() && std::find(prefixes.begin(),prefixes.end(),path)==prefixes.end())prefixes.push_back(path);};
        if(!prefix.empty())add(prefix);
        else {
            auto wine=env("WINEPREFIX"),compat=env("STEAM_COMPAT_DATA_PATH");
            if(!wine.empty())add(wine);
            else if(!compat.empty())add(Path(compat)/"pfx");
            else if(!game.empty())add(game.parent_path().parent_path()/"compatdata/322170/pfx");
            else if(!home.empty()){
                add(Path(home)/".local/share/Steam/steamapps/compatdata/322170/pfx");
                add(Path(home)/".steam/steam/steamapps/compatdata/322170/pfx");
                add(Path(home)/".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/compatdata/322170/pfx");
            }
        }
    }
    std::string resolve(std::string path) const {
        if(path.empty())return path;
        std::replace(path.begin(),path.end(),'\\','/');
        if(path.starts_with("//?/"))path.erase(0,4);
        if(path.size()>2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1]==':' && path[2]=='/'){
            auto drive=lower(path.substr(0,2));
            for(const auto& prefix:prefixes){
                auto root=prefix/"dosdevices"/drive;
                std::error_code ec;
                if(drive=="c:" && !std::filesystem::is_directory(root,ec))root=prefix/"drive_c";
                auto candidate=existing(root,Path(path.substr(3)));
                if(!candidate.empty())return candidate.string();
            }
            return path;
        }
        if(Path(path).is_absolute())return path;
        if(!game.empty()){
            auto candidate=existing(game,Path(path));
            if(!candidate.empty())return candidate.string();
        }
        return path;
    }
};
