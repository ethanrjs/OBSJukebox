#pragma once
// The operating-system calls the updater needs, kept out of Updater.cpp.
#include <filesystem>
#include <string>
#include <vector>

namespace update_platform {
// Any process named obs64.exe (Windows) or OBS (macOS), from any path.
bool obsRunning();
// An update setup holds its lock (spec 6.3 step 1).
bool setupWaiting();
// GD runs under Wine or Proton.
bool underWine();
// Some existing part of the path is a symlink, junction or other reparse point.
bool pathHasLink(const std::filesystem::path& path);
// Whether a setup started now would survive GD's exit (spec 6.4).
enum class Job { Free, Breakaway, Trapped };
Job job();
// Starts program with args, no shell. On macOS program is /bin/zsh and args[0] the script.
bool startSetup(const std::filesystem::path& program, const std::vector<std::string>& args, bool breakaway);
#ifdef __APPLE__
bool unzip(const std::filesystem::path& zip, const std::filesystem::path& into);
std::filesystem::path gdBundle();
#endif
} // namespace update_platform
