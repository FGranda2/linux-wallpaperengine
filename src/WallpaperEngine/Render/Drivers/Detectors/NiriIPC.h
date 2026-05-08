#pragma once

#ifdef ENABLE_WAYLAND

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace WallpaperEngine::Render::Drivers::Detectors {

/**
 * Minimal client for Niri's JSON-over-UNIX-socket IPC at $NIRI_SOCKET.
 *
 * Wire protocol: line-delimited JSON. Requests are bare-string tags
 * (e.g. "Outputs"). Successful replies are wrapped as
 * {"Ok": {"<tag>": <payload>}}. EventStream is a long-lived subscription
 * emitting one JSON object per line.
 */
class NiriIPC {
public:
    using EventHandler = std::function<void (const nlohmann::json&)>;

    /**
     * @return $NIRI_SOCKET if set and non-empty, else nullopt.
     */
    static std::optional<std::string> socketPath ();

    /**
     * Send a single bare-string request, read one reply line, and return the
     * inner payload from {"Ok": {"<tag>": <payload>}}. Returns nullopt on any
     * failure (connect, send, parse, schema mismatch).
     */
    static std::optional<nlohmann::json> snapshot (const std::string& tag);

    /**
     * Starts the EventStream worker thread. handler is invoked for each JSON
     * line received from the stream. Reconnects with exponential backoff on
     * disconnect.
     */
    explicit NiriIPC (EventHandler handler);
    ~NiriIPC ();

    NiriIPC (const NiriIPC&) = delete;
    NiriIPC& operator= (const NiriIPC&) = delete;

private:
    void workerLoop ();

    EventHandler m_handler;
    std::atomic<bool> m_stop {false};
    std::atomic<int> m_socketFd {-1};
    std::thread m_thread;
};

} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
