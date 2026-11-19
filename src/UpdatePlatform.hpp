#pragma once
// The OS calls the updater needs, kept out of Updater.cpp.
#include <filesystem>
#include <string>
#include <vector>

namespace update_platform {
// Any process named obs64.exe (Windows) or OBS (macOS), from any path.
bool obsRunning();
// An update setup holds its lock.
bool setupWaiting();
// GD runs under Wine or Proton.
bool underWine();
// Some existing part of the path is a symlink, junction or other reparse point.
bool pathHasLink(std::filesystem::path const& path);
// Whether a setup started now would survive GD's exit.
enum class Job {
    Free,
    Breakaway,
    Trapped
};
Job job();
// Starts program with args, no shell. On macOS program is /bin/zsh and args[0] the script.
bool startSetup(
    std::filesystem::path const& program, std::vector<std::string> const& args, bool breakaway
);
#ifdef __APPLE__
bool unzip(std::filesystem::path const& zip, std::filesystem::path const& into);
std::filesystem::path gdBundle();
#endif
}
