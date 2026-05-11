#pragma once

#include <glm/vec4.hpp>
#include <string>

namespace WallpaperEngine::Render::Drivers::Output {
class OutputViewport {
public:
    OutputViewport (glm::ivec4 viewport, std::string name, bool single = false);
    virtual ~OutputViewport () = default;

    glm::ivec4 viewport;
    std::string name;

    /** Whether this viewport is single in the framebuffer or shares space with more viewports */
    bool single;

    /**
     * Activates output's context for drawing
     */
    virtual void makeCurrent () = 0;

    /**
     * Swaps buffers to present data on the viewport
     */
    virtual void swapOutput () = 0;

    /**
     * Minimal work to keep the viewport's present cycle alive while its
     * wallpaper is paused. Backends driven by compositor frame callbacks
     * (Wayland) must override this to re-arm the callback and commit the
     * surface without doing any GL work; otherwise the callback chain stalls
     * and the viewport never gets another update event. Default is a no-op
     * for backends that drive frames externally (GLFW, X11).
     */
    virtual void pauseTick () { }
};
} // namespace WallpaperEngine::Render::Drivers::Output
