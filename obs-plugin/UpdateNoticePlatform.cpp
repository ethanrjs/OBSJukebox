#include "UpdateNoticePlatform.hpp"
#include <mutex>
#include <string_view>
#ifdef _WIN32
    #include <windows.h>
    #include <objbase.h>
    #include <shellapi.h>
    #include <winhttp.h>
#else
    #include <curl/curl.h>
    #include <gio/gio.h>
#endif

namespace update_notice::platform {
#ifdef _WIN32
namespace {
std::mutex mutex;
HINTERNET request = nullptr;
DWORD askingThread = 0;

std::wstring wide(std::string_view text) {
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    return out;
}

struct Handle {
    HINTERNET handle;

    ~Handle() {
        if (handle) WinHttpCloseHandle(handle);
    }
};
}

std::optional<std::string> get(std::string const& url, size_t cap) {
    auto address = wide(url);
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(address.c_str(), 0, 0, &parts)) return std::nullopt;
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength + parts.dwExtraInfoLength);
    Handle session{WinHttpOpen(
        L"OBS-Jukebox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    )};
    if (!session.handle || !WinHttpSetTimeouts(session.handle, 15000, 15000, 15000, 15000))
        return std::nullopt;
    Handle connection{WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0)};
    if (!connection.handle) return std::nullopt;
    HINTERNET opened = WinHttpOpenRequest(
        connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
    );
    if (!opened) return std::nullopt;
    {
        std::lock_guard lock(mutex);
        request = opened;
    }
    // cancel() closes the request handle from another thread, which WinHTTP documents as the way
    // to end a blocked synchronous call; each call below then fails.
    std::optional<std::string> body;
    DWORD status = 0, size = sizeof(status);
    if (WinHttpSendRequest(opened, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(opened, nullptr) &&
        WinHttpQueryHeaders(
            opened, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX
        ) &&
        status == 200) {
        std::string data;
        char buffer[4096];
        DWORD read = 0;
        while (data.size() <= cap && WinHttpReadData(opened, buffer, sizeof(buffer), &read)) {
            if (!read) {
                body = std::move(data);
                break;
            }
            data.append(buffer, read);
        }
    }
    std::lock_guard lock(mutex);
    if (request) WinHttpCloseHandle(request);
    request = nullptr;
    return body;
}

bool ask(Notice const& notice) {
    auto text = wide(
        "OBS Jukebox " + notice.latest + " is out for Geometry Dash " + notice.latestGd +
        ".\n\nYour version (" + notice.running + ") only works with GD " + notice.runningGd +
        ", so the game mod stops loading after the GD update. Install " + notice.latest +
        " to get it working again.\n\nOpen the download page?"
    );
    {
        std::lock_guard lock(mutex);
        askingThread = GetCurrentThreadId();
    }
    auto answer = MessageBoxW(
        nullptr, text.c_str(), L"OBS Jukebox",
        MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND
    );
    std::lock_guard lock(mutex);
    askingThread = 0;
    return answer == IDYES;
}

void cancel() {
    std::lock_guard lock(mutex);
    if (request) WinHttpCloseHandle(request);
    request = nullptr;
    if (askingThread)
        EnumThreadWindows(
            askingThread,
            [](HWND window, LPARAM) {
                PostMessageW(window, WM_COMMAND, IDNO, 0);
                return TRUE;
            },
            0
        );
}

void open(char const* url) {
    auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(com)) CoUninitialize();
}
#else
namespace {
std::mutex mutex;
std::atomic<bool> cancelled = false;
GMainLoop* waiting = nullptr;

struct Download {
    std::string body;
    size_t cap;
};

struct Answer {
    guint32 id = 0;
    bool ended = false, open = false;
    GMainLoop* loop;
};

size_t append(char* data, size_t size, size_t count, void* user) {
    auto& download = *static_cast<Download*>(user);
    download.body.append(data, size * count);
    return download.body.size() > download.cap ? 0 : size * count;
}

int progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return cancelled ? 1 : 0;
}

void onSignal(
    GDBusConnection*, char const*, char const*, char const*, char const* signal,
    GVariant* parameters, gpointer user
) {
    auto& answer = *static_cast<Answer*>(user);
    guint32 id = 0;
    char const* action = "";
    if (std::string_view(signal) == "ActionInvoked") g_variant_get(parameters, "(u&s)", &id, &action);
    else g_variant_get_child(parameters, 0, "u", &id);
    if (id != answer.id) return;
    answer.ended = true;
    answer.open = std::string_view(action) == "open";
    g_main_loop_quit(answer.loop);
}

GVariant* notify(GDBusConnection* bus, char const* method, GVariant* parameters, GError** error) {
    return g_dbus_connection_call_sync(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000,
        nullptr, error
    );
}
}

std::optional<std::string> get(std::string const& url, size_t cap) {
    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;
    Download download{{}, cap};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "OBS-Jukebox");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    long status = 0;
    bool ok = curl_easy_perform(curl) == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK && status == 200;
    curl_easy_cleanup(curl);
    if (!ok) return std::nullopt;
    return std::move(download.body);
}

// A desktop notification through org.freedesktop.Notifications. Flatpak OBS may talk to it (its
// manifest grants --talk-name=org.freedesktop.Notifications).
bool ask(Notice const& notice) {
    auto context = g_main_context_new();
    g_main_context_push_thread_default(context);
    Answer answer;
    answer.loop = g_main_loop_new(context, false);
    GError* error = nullptr;
    if (auto bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error)) {
        auto subscription = g_dbus_connection_signal_subscribe(
            bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications", nullptr,
            "/org/freedesktop/Notifications", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onSignal, &answer,
            nullptr
        );
        GVariantBuilder actions, hints;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&actions, "s", "open");
        g_variant_builder_add(&actions, "s", "Open download page");
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        auto summary = "OBS Jukebox " + notice.latest + " is out for GD " + notice.latestGd;
        auto body = "Your version only works with GD " + notice.runningGd + ". Install " +
            notice.latest + " to get the mod working again.";
        auto reply = notify(
            bus, "Notify",
            g_variant_new(
                "(susssasa{sv}i)", "OBS Jukebox", 0u, "com.obsproject.Studio", summary.c_str(),
                body.c_str(), &actions, &hints, -1
            ),
            &error
        );
        if (reply) {
            g_variant_get(reply, "(u)", &answer.id);
            g_variant_unref(reply);
            bool run = false;
            {
                std::lock_guard lock(mutex);
                run = !cancelled;
                if (run) waiting = answer.loop;
            }
            if (run) g_main_loop_run(answer.loop);
            {
                std::lock_guard lock(mutex);
                waiting = nullptr;
            }
            // OBS is closing; a button left behind would open nothing.
            if (!answer.ended)
                if (auto closed = notify(bus, "CloseNotification", g_variant_new("(u)", answer.id), nullptr))
                    g_variant_unref(closed);
        }
        g_dbus_connection_signal_unsubscribe(bus, subscription);
        g_object_unref(bus);
    }
    g_clear_error(&error);
    g_main_loop_unref(answer.loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    return answer.open;
}

void cancel() {
    std::lock_guard lock(mutex);
    cancelled = true;
    if (waiting) g_main_loop_quit(waiting);
}

void open(char const* url) {
    g_app_info_launch_default_for_uri(url, nullptr, nullptr);
}
#endif
}
