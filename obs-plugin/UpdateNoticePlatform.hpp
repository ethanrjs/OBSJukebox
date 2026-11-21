#pragma once
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>

namespace update_notice::platform {
struct Notice {
    std::string latest, latestGd, running, runningGd;
};

std::optional<std::string> get(std::string const& url, size_t cap);
std::optional<bool> ask(Notice const& notice);
void cancel();
void open(char const* url);
}
