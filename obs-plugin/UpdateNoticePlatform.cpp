#include "UpdateNoticePlatform.hpp"
#include <mutex>
#include <string_view>
#include <utility>
#ifdef _WIN32
    #include <windows.h>
    #include <objbase.h>
    #include <shellapi.h>
    #include <winhttp.h>
#else
    #include <curl/curl.h>
    #include <dlfcn.h>
    #include <gio/gio.h>
#endif

namespace update_notice::platform {
#ifdef _WIN32
namespace {
// Recursive: WinHTTP may run the next callback inside the call that issues it, on this thread.
std::recursive_mutex mutex;
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

struct Fetch {
    std::string body;
    size_t cap;
    HANDLE closed;
    bool complete = false;
    char buffer[4096];
};

// The request closes exactly once, from a callback or from cancel(); its closing callback is the
// last one, so get() waits for it before the plugin can unload.
void close(HINTERNET handle) {
    {
        std::lock_guard lock(mutex);
        if (request != handle) return;
        request = nullptr;
    }
    WinHttpCloseHandle(handle);
}

template <class Call>
void next(HINTERNET handle, Call call) {
    std::unique_lock lock(mutex);
    if (request != handle) return;
    if (call()) return;
    lock.unlock();
    close(handle);
}

void CALLBACK onStatus(HINTERNET handle, DWORD_PTR context, DWORD status, LPVOID, DWORD length) {
    auto& fetch = *reinterpret_cast<Fetch*>(context);
    auto read = [&] {
        return WinHttpReadData(handle, fetch.buffer, sizeof(fetch.buffer), nullptr);
    };
    switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            next(handle, [&] { return WinHttpReceiveResponse(handle, nullptr); });
            break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(
                handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX
            );
            if (code == 200) next(handle, read);
            else close(handle);
            break;
        }
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            if (!length) fetch.complete = true;
            else fetch.body.append(fetch.buffer, length);
            if (!length || fetch.body.size() > fetch.cap) close(handle);
            else next(handle, read);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: close(handle); break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: SetEvent(fetch.closed); break;
    }
}
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
        WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC
    )};
    if (!session.handle || !WinHttpSetTimeouts(session.handle, 15000, 15000, 15000, 15000))
        return std::nullopt;
    Handle connection{WinHttpConnect(session.handle, host.c_str(), parts.nPort, 0)};
    if (!connection.handle) return std::nullopt;
    Handle opened{WinHttpOpenRequest(
        connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0
    )};
    Fetch fetch{{}, cap, CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    auto context = reinterpret_cast<DWORD_PTR>(&fetch);
    if (!opened.handle || !fetch.closed ||
        !WinHttpSetOption(opened.handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(
            opened.handle, onStatus,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0
        ) == WINHTTP_INVALID_STATUS_CALLBACK) {
        if (fetch.closed) CloseHandle(fetch.closed);
        return std::nullopt;
    }
    auto handle = std::exchange(opened.handle, nullptr);
    {
        std::lock_guard lock(mutex);
        request = handle;
    }
    next(handle, [&] {
        return WinHttpSendRequest(
            handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, context
        );
    });
    WaitForSingleObject(fetch.closed, INFINITE);
    CloseHandle(fetch.closed);
    if (!fetch.complete) return std::nullopt;
    return std::move(fetch.body);
}

std::optional<bool> ask(Notice const& notice) {
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
    if (!answer) return std::nullopt;
    return answer == IDYES;
}

void cancel() {
    HINTERNET pending = nullptr;
    {
        std::lock_guard lock(mutex);
        pending = request;
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
    if (pending) close(pending);
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
GCancellable* connecting = nullptr;
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

GVariant* notifications(
    GDBusConnection* bus, char const* method, GVariant* parameters, int timeout,
    GCancellable* cancellable, GError** error
) {
    return g_dbus_connection_call_sync(
        bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE,
        timeout, cancellable, error
    );
}

guint subscribe(GDBusConnection* bus, char const* signal, Answer& answer) {
    return g_dbus_connection_signal_subscribe(
        bus, "org.freedesktop.Notifications", "org.freedesktop.Notifications", signal,
        "/org/freedesktop/Notifications", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, onSignal, &answer,
        nullptr
    );
}

// A private connection that never exits OBS when the bus goes away, unlike the shared one.
GDBusConnection* connect(GCancellable* cancellable, GError** error) {
    auto address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancellable, error);
    if (!address) return nullptr;
    auto bus = g_dbus_connection_new_for_address_sync(
        address,
        GDBusConnectionFlags(
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
        ),
        nullptr, cancellable, error
    );
    g_free(address);
    if (bus) g_dbus_connection_set_exit_on_close(bus, false);
    return bus;
}
}

// Loaded at run time: OBS already has libcurl loaded, and linking it would refuse to load the
// whole plugin on a distribution whose libcurl uses other symbol versions.
std::optional<std::string> get(std::string const& url, size_t cap) {
    static auto library = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!library) return std::nullopt;
    auto init = reinterpret_cast<CURL* (*)()>(dlsym(library, "curl_easy_init"));
    auto set = reinterpret_cast<CURLcode (*)(CURL*, CURLoption, ...)>(dlsym(library, "curl_easy_setopt"));
    auto perform = reinterpret_cast<CURLcode (*)(CURL*)>(dlsym(library, "curl_easy_perform"));
    auto info = reinterpret_cast<CURLcode (*)(CURL*, CURLINFO, ...)>(dlsym(library, "curl_easy_getinfo"));
    auto cleanup = reinterpret_cast<void (*)(CURL*)>(dlsym(library, "curl_easy_cleanup"));
    if (!init || !set || !perform || !info || !cleanup) return std::nullopt;
    CURL* curl = init();
    if (!curl) return std::nullopt;
    Download download{{}, cap};
    set(curl, CURLOPT_URL, url.c_str());
    set(curl, CURLOPT_FOLLOWLOCATION, 1L);
    set(curl, CURLOPT_TIMEOUT, 15L);
    set(curl, CURLOPT_NOSIGNAL, 1L);
    set(curl, CURLOPT_USERAGENT, "OBS-Jukebox");
    set(curl, CURLOPT_WRITEFUNCTION, append);
    set(curl, CURLOPT_WRITEDATA, &download);
    set(curl, CURLOPT_NOPROGRESS, 0L);
    set(curl, CURLOPT_XFERINFOFUNCTION, progress);
    long status = 0;
    bool ok = perform(curl) == CURLE_OK && info(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
        status == 200;
    cleanup(curl);
    if (!ok) return std::nullopt;
    return std::move(download.body);
}

// A desktop notification through org.freedesktop.Notifications. Flatpak OBS may talk to it (its
// manifest grants --talk-name=org.freedesktop.Notifications).
std::optional<bool> ask(Notice const& notice) {
    auto cancellable = g_cancellable_new();
    {
        std::lock_guard lock(mutex);
        if (cancelled) g_cancellable_cancel(cancellable);
        connecting = cancellable;
    }
    auto context = g_main_context_new();
    g_main_context_push_thread_default(context);
    Answer answer;
    answer.loop = g_main_loop_new(context, false);
    bool shown = false;
    GError* error = nullptr;
    if (auto bus = connect(cancellable, &error)) {
        guint subscriptions[] = {
            subscribe(bus, "ActionInvoked", answer), subscribe(bus, "NotificationClosed", answer)
        };
        GVariantBuilder actions, hints;
        g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
        g_variant_builder_add(&actions, "s", "open");
        g_variant_builder_add(&actions, "s", "Open download page");
        g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
        auto summary = "OBS Jukebox " + notice.latest + " is out for GD " + notice.latestGd;
        auto body = "Your version only works with GD " + notice.runningGd + ". Install " +
            notice.latest + " to get the mod working again.";
        auto reply = notifications(
            bus, "Notify",
            g_variant_new(
                "(susssasa{sv}i)", "OBS Jukebox", 0u, "com.obsproject.Studio", summary.c_str(),
                body.c_str(), &actions, &hints, -1
            ),
            5000, cancellable, &error
        );
        if (reply) {
            shown = true;
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
                if (auto closed = notifications(
                        bus, "CloseNotification", g_variant_new("(u)", answer.id), 1000, nullptr, nullptr
                    ))
                    g_variant_unref(closed);
        }
        for (auto subscription : subscriptions)
            g_dbus_connection_signal_unsubscribe(bus, subscription);
        g_dbus_connection_close(bus, nullptr, nullptr, nullptr);
        g_object_unref(bus);
    }
    g_clear_error(&error);
    g_main_loop_unref(answer.loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);
    {
        std::lock_guard lock(mutex);
        connecting = nullptr;
    }
    g_object_unref(cancellable);
    if (!shown) return std::nullopt;
    return answer.open;
}

void cancel() {
    std::lock_guard lock(mutex);
    cancelled = true;
    if (connecting) g_cancellable_cancel(connecting);
    if (waiting) g_main_loop_quit(waiting);
}

void open(char const* url) {
    g_app_info_launch_default_for_uri(url, nullptr, nullptr);
}
#endif
}
