#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>

namespace update_notice::platform {
struct Notice {
    std::string latest, latestGd, running, runningGd;
};

// An HTTPS GET of at most `cap` bytes that follows redirects. cancel() ends it early.
std::optional<std::string> get(std::string const& url, size_t cap);
// Shows the notice and waits for the player; true means open the download page.
bool ask(Notice const& notice);
// Ends a get() or ask() running on another thread. One that hasn't started yet still runs, so
// the caller repeats this until its worker finishes.
void cancel();
void open(char const* url);
}
