#pragma once

#ifdef ENABLE_WAYLAND

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "FullScreenDetector.h"

namespace WallpaperEngine::Render::Drivers::Detectors {

class NiriIPC;

/**
 * Pause-on-fullscreen detector for the Niri compositor.
 *
 * Niri does not mark its tiled windows as FULLSCREEN/MAXIMIZED in
 * wlr-foreign-toplevel-management, so the standard Wayland detector cannot
 * detect when wallpaper is occluded by a regular tiled column. Instead this
 * detector queries Niri's IPC, sums tile_size across each output's active
 * workspace independently, and reports per-output coverage when the aggregate
 * meets the threshold for that specific output.
 *
 * Each output's coverage is debounced independently so workspace changes on
 * one monitor never delay or suppress transitions on another.
 */
class NiriFullScreenDetector final : public FullScreenDetector {
public:
    explicit NiriFullScreenDetector (Application::ApplicationContext& appContext);
    ~NiriFullScreenDetector () override;

    [[nodiscard]] bool anythingFullscreen () const override;
    [[nodiscard]] std::optional<std::unordered_set<std::string>> coveredOutputs () const override;
    void reset () override;

private:
    struct DebounceState {
        bool raw = false;
        bool debounced = false;
        std::chrono::steady_clock::time_point lastChange {};
    };

    void recompute ();
    void advanceDebounceLocked () const;

    mutable std::mutex m_mutex;
    mutable std::unordered_map<std::string, DebounceState> m_perOutput;

    std::unique_ptr<NiriIPC> m_ipc;
};

} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
