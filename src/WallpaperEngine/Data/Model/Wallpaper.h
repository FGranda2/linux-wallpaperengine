#pragma once

#include <array>
#include <memory>

#include <glm/glm.hpp>
#include <utility>

#include "Object.h"
#include "Types.h"
#include "WallpaperEngine/Data/Utils/TypeCaster.h"

namespace WallpaperEngine::Data::Model {
using namespace WallpaperEngine::Data::Utils;

struct WallpaperData {
    std::string filename;
    Project& project;
};

class Wallpaper : public TypeCaster, public WallpaperData {
public:
    explicit Wallpaper (WallpaperData data) noexcept : TypeCaster (), WallpaperData (std::move (data)) { };
    ~Wallpaper () override = default;
};

class Video final : public Wallpaper {
public:
    explicit Video (WallpaperData data) noexcept : Wallpaper (std::move (data)) { }

    ~Video () override = default;
};

class Web final : public Wallpaper {
public:
    explicit Web (WallpaperData data) noexcept : Wallpaper (std::move (data)) { }

    ~Web () override = default;
};

struct SceneData {
    struct {
	glm::vec3 ambient;
	glm::vec3 skylight;
	UserSettingUniquePtr clear;
    } colors;

    /**
     * Approximation of the scene's light sources for LIGHTING-enabled materials.
     *
     * Full per-light shading (shadows, normals, specular) is not implemented; instead every
     * point/tube light is reduced to a radially attenuated colour contribution evaluated
     * per-pixel by an injected PerformLighting_V1 (see ShaderUnit::preprocessRequires).
     * Colours are premultiplied by intensity and peak-normalised on the CPU so the brightest
     * lit spot of the scene plane reaches full strength without saturating everything.
     */
    struct ApproxLight {
	/** xyz = light position in scene-centred coordinates (x right, y up, z depth), w = radius */
	glm::vec4 position { 0.0f };
	/** rgb = colour * intensity, peak-normalised across the scene; w = falloff exponent */
	glm::vec4 color { 0.0f };
    };
    static constexpr std::size_t MaxApproxLights = 4;
    struct {
	std::array<ApproxLight, MaxApproxLights> slots;
	/** Number of used slots; 0 means no lights and LIGHTING materials render plain albedo */
	int count;
    } approxLights;
    /**
     * Camera configuration
     */
    struct Camera {
	/** Enable fade effect */
	bool fade;
	/** Used by the software to allow the users to preview the background or not? */
	bool preview;

	/**
	 * Bloom effect configuration
	 */
	struct {
	    /** If bloom is enabled or not */
	    UserSettingUniquePtr enabled;
	    /** Bloom's strength to pass onto the shader */
	    UserSettingUniquePtr strength;
	    /** Bloom's threshold to pass onto the shader */
	    UserSettingUniquePtr threshold;
	} bloom;
	/**
	 * Parallax effect configuration
	 */
	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr amount;
	    UserSettingUniquePtr delay;
	    UserSettingUniquePtr mouseInfluence;
	} parallax;

	/**
	 * Shake effect configuration
	 */
	struct {
	    UserSettingUniquePtr enabled;
	    UserSettingUniquePtr amplitude;
	    UserSettingUniquePtr roughness;
	    UserSettingUniquePtr speed;
	} shake;

	/**
	 * Position configuration
	 */
	struct {
	    glm::vec3 center;
	    glm::vec3 eye;
	    glm::vec3 up;
	} configuration;

	/**
	 * Projection information
	 */
	struct {
	    int width;
	    int height;
	    bool isAuto;
	    float nearz;
	    float farz;
	    float fov;
	} projection;

	/**
	 * Scene zoom factor (general.zoom). Values above 1 render the scene zoomed in, giving the
	 * edges extra margin so camera parallax displacement never exposes layer borders.
	 */
	float zoom;
    } camera;

    ObjectList objects;
};

class Scene final : public Wallpaper, public SceneData {
public:
    explicit Scene (WallpaperData data, SceneData sceneData) noexcept :
	Wallpaper (std::move (data)), SceneData (std::move (sceneData)) { }

    ~Scene () override = default;
};
} // namespace WallpaperEngine::Data::Model
