#pragma once

#include <optional>
#include <string>
#include <unordered_set>

#include "WallpaperEngine/Application/ApplicationContext.h"

namespace WallpaperEngine::Render::Drivers::Detectors {
class FullScreenDetector {
public:
    explicit FullScreenDetector (Application::ApplicationContext& appContext);
    virtual ~FullScreenDetector () = default;

    /**
     * @return If anything is fullscreen
     */
    [[nodiscard]] virtual bool anythingFullscreen () const;
    /**
     * Per-output coverage state for detectors that can distinguish individual outputs.
     *
     * @return std::nullopt if the detector cannot answer per-output (callers should
     *         fall back to anythingFullscreen() applied globally); otherwise the
     *         exact set of output names that are currently considered "covered".
     *         An empty set means "nothing is covered on any output".
     */
    [[nodiscard]] virtual std::optional<std::unordered_set<std::string>> coveredOutputs () const;
    /**
     * Restarts the fullscreen detector, specially useful if there's any resources tied to the output driver
     */
    virtual void reset ();
    /**
     * @return The application context using this detector
     */
    [[nodiscard]] Application::ApplicationContext& getApplicationContext () const;

private:
    Application::ApplicationContext& m_applicationContext;
};
} // namespace WallpaperEngine::Render::Drivers::Detectors