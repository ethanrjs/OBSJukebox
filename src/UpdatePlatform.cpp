#include "UpdatePlatform.hpp"
#include "UpdateRules.hpp"
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#include <tlhelp32.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <cstdlib>
#include <libproc.h>
#include <spawn.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace update_platform {
#ifdef _WIN32
bool obsRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof entry};
    bool found = false;
    for (bool more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        found = _wcsicmp(entry.szExeFile, L"obs64.exe") == 0;
    CloseHandle(snapshot);
    return found;
}
bool setupWaiting() {
    HANDLE lock = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\OBSJukeboxUpdate");
    if (!lock) return false;
    CloseHandle(lock);
    return true;
}
bool underWine() {
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}
bool pathHasLink(const std::filesystem::path& path) {
    for (auto part = path; !part.empty() && part != part.parent_path(); part = part.parent_path()) {
        DWORD attributes = GetFileAttributesW(part.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    }
    return false;
}
Job job() {
    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) return Job::Free;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    // Unreadable limits: assume the worst, so GD never exits for a setup that would die with it.
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &info, sizeof info, nullptr)) return Job::Trapped;
    auto flags = info.BasicLimitInformation.LimitFlags;
    if (!(flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) || (flags & JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)) return Job::Free;
    return flags & JOB_OBJECT_LIMIT_BREAKAWAY_OK ? Job::Breakaway : Job::Trapped;
}
bool startSetup(const std::filesystem::path& program, const std::vector<std::string>& args, bool breakaway) {
    std::wstring command = update_rules::quoteWindowsArgument(program.wstring());
    for (const auto& arg : args) command += L" " + update_rules::quoteWindowsArgument(std::filesystem::path(reinterpret_cast<const char8_t*>(arg.c_str())).wstring());
    STARTUPINFOW startup{sizeof startup};
    PROCESS_INFORMATION process{};
    DWORD flags = CREATE_NO_WINDOW | (breakaway ? CREATE_BREAKAWAY_FROM_JOB : 0);
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &startup, &process)) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
#else
bool obsRunning() {
    int count = proc_listallpids(nullptr, 0);
    if (count <= 0) return false;
    std::vector<pid_t> pids(count + 32);
    count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    char name[2 * MAXCOMLEN + 1];
    for (int i = 0; i < count; ++i)
        if (proc_name(pids[i], name, sizeof name) > 0 && std::strcmp(name, "OBS") == 0) return true;
    return false;
}
bool setupWaiting() {
    auto file = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") / "Library/Application Support/OBS Jukebox/update.lock";
    int fd = open(file.c_str(), O_RDWR);
    if (fd < 0) return false;
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    bool held = fcntl(fd, F_SETLK, &lock) == -1 && (errno == EAGAIN || errno == EACCES);
    close(fd); // also drops the test lock if it was taken
    return held;
}
bool underWine() { return false; }
bool pathHasLink(const std::filesystem::path& path) {
    for (auto part = path; !part.empty() && part != part.parent_path(); part = part.parent_path()) {
        struct stat info{};
        if (lstat(part.c_str(), &info) == 0 && S_ISLNK(info.st_mode)) return true;
    }
    return false;
}
Job job() { return Job::Free; }
static bool spawn(const char* program, std::vector<std::string> args, pid_t* child, bool detach) {
    std::vector<char*> argv{const_cast<char*>(program)};
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    if (detach) posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    int error = posix_spawn(child, program, &actions, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    return error == 0;
}
bool startSetup(const std::filesystem::path& program, const std::vector<std::string>& args, bool) {
    pid_t child;
    return spawn(program.c_str(), args, &child, true);
}
bool unzip(const std::filesystem::path& zip, const std::filesystem::path& into) {
    pid_t child;
    if (!spawn("/usr/bin/ditto", {"-x", "-k", zip.string(), into.string()}, &child, false)) return false;
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
std::filesystem::path gdBundle() {
    char path[PATH_MAX];
    CFURLRef url = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    bool ok = url && CFURLGetFileSystemRepresentation(url, true, reinterpret_cast<UInt8*>(path), sizeof path);
    if (url) CFRelease(url);
    return ok ? std::filesystem::path(path) : std::filesystem::path{};
}
#endif
} // namespace update_platform
