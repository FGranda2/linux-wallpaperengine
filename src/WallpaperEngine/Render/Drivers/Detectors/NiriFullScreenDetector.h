#pragma once

#ifdef ENABLE_WAYLAND

#include <chrono>
#include <memory>
#include <mutex>

#include "FullScreenDetector.h"

namespace WallpaperEngine::Render::Drivers::Detectors {

class NiriIPC;

/**
 * Pause-on-fullscreen detector for the Niri compositor.
 *
 * Niri does not mark its tiled windows as FULLSCREEN/MAXIMIZED in
 * wlr-foreign-toplevel-management, so the standard Wayland detector cannot
 * detect when wallpaper is occluded by a regular tiled column. Instead this
 * detector queries Niri's IPC, sums tile_size across all windows on each
 * output's active workspace, and reports "covered" when the aggregate area
 * meets a configurable threshold of the output's logical area on every
 * managed output.
 */
class NiriFullScreenDetector final : public FullScreenDetector {
public:
    explicit NiriFullScreenDetector (Application::ApplicationContext& appContext);
    ~NiriFullScreenDetector () override;

    [[nodiscard]] bool anythingFullscreen () const override;
    void reset () override;

private:
    void recompute ();

    mutable std::mutex m_mutex;
    bool m_rawCovered = false;
    mutable bool m_debouncedCovered = false;
    mutable std::chrono::steady_clock::time_point m_lastChange;

    std::unique_ptr<NiriIPC> m_ipc;
};

} // namespace WallpaperEngine::Render::Drivers::Detectors

#endif /* ENABLE_WAYLAND */
