#include "UpdateNotice.hpp"
#include "UpdateNoticePlatform.hpp"
#include "../src/UpdateRules.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <system_error>
#include <thread>
#include <util/bmem.h>
#include <util/platform.h>

namespace update_notice {
namespace {
namespace ur = update_rules;
constexpr auto DOWNLOAD_PAGE = "https://github.com/navinor/OBSJukebox/releases/latest";

std::mutex mutex;
std::condition_variable wake;
bool stopping = false;
std::optional<ur::Manifest> latest;
std::atomic<bool> finished = false;
std::thread worker;

// Tests point this at a local server. The button only ever opens the real releases page, so
// another manifest can change the text but not where the player goes.
std::string manifestUrl() {
    if (auto base = std::getenv("OBS_JUKEBOX_UPDATE_BASE_URL"))
        return std::string(base) + "/latest/download/update.json";
#ifdef OBS_JUKEBOX_QA
    return {};
#else
    return "https://github.com/navinor/OBSJukebox/releases/latest/download/update.json";
#endif
}

bool outputActive() {
    bool active = false;
    obs_enum_outputs(
        [](void* param, obs_output_t* output) {
            if (!obs_output_active(output)) return true;
            *static_cast<bool*>(param) = true;
            return false;
        },
        &active
    );
    return active;
}

// True the first time this version comes up, which it records.
bool firstShowing(ur::Version version) {
    auto text = ur::toString(version);
    char* folder = obs_module_config_path("");
    char* file = obs_module_config_path("update-notice");
    bool first = false;
    if (folder && file && os_mkdirs(folder) != MKDIR_ERROR) {
        char* shown = os_quick_read_utf8_file(file);
        first = !shown || text != shown;
        if (first) os_quick_write_utf8_file(file, text.data(), text.size(), false);
        bfree(shown);
    }
    bfree(folder);
    bfree(file);
    return first;
}

void check(ur::Version running) {
    {
        std::unique_lock lock(mutex);
        // Long enough for OBS's window to open first, so the dialog lands on top of it.
        if (wake.wait_for(lock, std::chrono::seconds(5), [] { return stopping; })) return;
    }
    auto body = platform::get(manifestUrl(), 16 * 1024);
    auto manifest = body ? ur::parseManifest(*body) : std::nullopt;
    if (!manifest || !ur::needsOtherGd(*manifest, running, OBS_JUKEBOX_GD)) return;
    {
        std::lock_guard lock(mutex);
        if (stopping) return;
        latest = manifest;
    }
    if (outputActive() || !firstShowing(manifest->version)) return;
    platform::Notice notice{
        ur::toString(manifest->version), manifest->gd, ur::toString(running), OBS_JUKEBOX_GD
    };
    if (platform::ask(notice)) platform::open(DOWNLOAD_PAGE);
}
}

void start() {
    auto running = ur::parseBuildVersion(OBS_JUKEBOX_VERSION);
    if (!running || !*OBS_JUKEBOX_GD || manifestUrl().empty()) return;
    try {
        worker = std::thread([version = *running] {
            check(version);
            finished = true;
        });
    }
    catch (std::system_error const& error) {
        blog(LOG_WARNING, "[OBS Jukebox] Could not start the update check: %s", error.what());
    }
}

void stop() {
    if (!worker.joinable()) return;
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    while (!finished) {
        platform::cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    worker.join();
}

bool addProperties(obs_properties_t* props) {
    std::optional<ur::Manifest> found;
    {
        std::lock_guard lock(mutex);
        found = latest;
    }
    if (!found) return false;
    auto text = "OBS Jukebox " + ur::toString(found->version) + " is out for Geometry Dash " +
        found->gd + ". This version only works with GD " OBS_JUKEBOX_GD ".";
    auto warning = obs_properties_add_text(props, "update", text.c_str(), OBS_TEXT_INFO);
    obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);
    obs_properties_add_button2(
        props,
        "update-download",
        "Open download page",
        [](obs_properties_t*, obs_property_t*, void*) {
            platform::open(DOWNLOAD_PAGE);
            return false;
        },
        nullptr
    );
    return true;
}
}
