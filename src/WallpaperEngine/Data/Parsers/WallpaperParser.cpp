#include "WallpaperParser.h"

#include <algorithm>
#include <cmath>

#include "ObjectParser.h"
#include "WallpaperEngine/Data/Model/Project.h"
#include "WallpaperEngine/Data/Model/Wallpaper.h"
#include "WallpaperEngine/FileSystem/Container.h"
#include "WallpaperEngine/Logging/Log.h"

using namespace WallpaperEngine::Data::Parsers;

namespace {
using ApproxLights = decltype (SceneData::approxLights);

/**
 * Radial falloff of an approximate light at a given distance (see computeApproxLights).
 */
float approxLightFalloff (const float distance, const float radius, const float exponent) {
    const float linear = std::clamp (1.0f - distance / std::max (radius, 1.0f), 0.0f, 1.0f);
    return std::pow (linear, exponent);
}

/**
 * Collects the scene's light objects into the per-pixel lighting approximation consumed by the
 * injected PerformLighting_V1 (see ShaderUnit::preprocessRequires).
 *
 * Per-light shading (shadows, normals, specular) is not implemented. Each point/tube light is
 * reduced to a radially attenuated colour: contribution = colour * intensity * (1 - d/radius)^exponent.
 * Tube lights are approximated by a point at their segment midpoint. Positions are converted from
 * screen coordinates (origin top-left, y down) to the scene-centred space shader vertices use
 * (x right, y up), keeping the authored z as depth.
 *
 * Real lighting is additive and the authored intensities (often 10+) would saturate to white, so the
 * summed contribution is peak-normalised on the CPU: colours are scaled so the brightest point of the
 * z=0 scene plane reaches channel value 1. This preserves each light's local hue (e.g. a pink corner
 * against a twilight-blue sky) instead of collapsing everything into one global tint.
 *
 * @param objects The scene's "objects" array.
 * @param width The scene's orthogonal projection width.
 * @param height The scene's orthogonal projection height.
 * @return The light approximation; count = 0 when the scene has no lights (materials render plain albedo).
 */
ApproxLights computeApproxLights (const JSON& objects, const float width, const float height) {
    ApproxLights result {};
    result.count = 0;

    for (const auto& cur : objects) {
	if (cur.find ("light") == cur.end ())
	    continue;

	const float intensity = cur.optional<float> ("intensity", 1.0f);
	if (intensity <= 0.0f)
	    continue;

	if (result.count >= static_cast<int> (SceneData::MaxApproxLights))
	    break;

	glm::vec3 origin = cur.optional<glm::vec3> ("origin", glm::vec3 (0.0f));
	// Tube lights span from origin towards origin + controlpoint; use the segment midpoint
	if (cur.find ("controlpoint") != cur.end ())
	    origin += cur.optional<glm::vec3> ("controlpoint", glm::vec3 (0.0f)) / 2.0f;

	auto& slot = result.slots[result.count++];
	slot.position = glm::vec4 (
	    origin.x - width / 2.0f, height / 2.0f - origin.y, origin.z,
	    cur.optional<float> ("radius", 1000.0f)
	);
	slot.color = glm::vec4 (
	    cur.optional<glm::vec3> ("color", glm::vec3 (1.0f)) * intensity,
	    std::max (cur.optional<float> ("exponent", 1.0f), 0.0f)
	);
    }

    if (result.count == 0)
	return result;

    // Peak-normalise: find the brightest summed channel over a grid of the z=0 scene plane
    float peak = 0.0f;
    constexpr int GRID = 16;
    for (int gy = 0; gy <= GRID; gy++) {
	for (int gx = 0; gx <= GRID; gx++) {
	    const glm::vec3 point (
		(static_cast<float> (gx) / GRID - 0.5f) * width, (static_cast<float> (gy) / GRID - 0.5f) * height,
		0.0f
	    );

	    glm::vec3 sum (0.0f);
	    for (int i = 0; i < result.count; i++) {
		const auto& light = result.slots[i];
		const float falloff
		    = approxLightFalloff (glm::distance (glm::vec3 (light.position), point), light.position.w, light.color.w);
		sum += glm::vec3 (light.color) * falloff;
	    }

	    peak = std::max ({peak, sum.r, sum.g, sum.b});
	}
    }

    if (peak > 0.0f) {
	// Calibration headroom: strict peak-normalisation renders noticeably darker than Wallpaper
	// Engine (calibrated against this scene's preview), whose additive lighting lets areas near
	// lights clip. Allow the brightest spot to reach 1.5 so mid-distance areas land at the
	// authored brightness; the clipping near lights matches the reference behaviour.
	constexpr float HEADROOM = 1.8f;
	for (int i = 0; i < result.count; i++) {
	    const float exponent = result.slots[i].color.w;
	    result.slots[i].color *= HEADROOM / peak;
	    result.slots[i].color.w = exponent;
	}
    }

    return result;
}
} // namespace

WallpaperUniquePtr WallpaperParser::parse (const JSON& file, Project& project) {
    switch (project.type) {
	case Project::Type_Scene:
	    return parseScene (file, project);
	case Project::Type_Video:
	    return parseVideo (file, project);
	case Project::Type_Web:
	    return parseWeb (file, project);
	default:
	    sLog.exception ("Unexpected project type value found... This is likely a bug");
    }
}

SceneUniquePtr WallpaperParser::parseScene (const JSON& file, Project& project) {
    const auto scene = JSON::parse (project.assetLocator->readString (file));
    const auto camera = scene.require ("camera", "Scenes must have a camera section");
    const auto general = scene.require ("general", "Scenes must have a general section");
    const auto projection
	= general.require ("orthogonalprojection", "General section must have orthogonal projection info");
    const auto objects = scene.require ("objects", "Scenes must have an objects section");
    const auto& properties = project.properties;

    // TODO: FIND IF THESE DEFAULTS ARE SENSIBLE OR NOT AND PERFORM PROPER VALIDATION WHEN CAMERA PREVIEW AND CAMERA
    // PARALLAX ARE PRESENT

    return std::make_unique <Scene> (
        WallpaperData {
            .filename = "",
            .project = project
        }, SceneData {
            .colors = {
                .ambient  = general.optional ("ambientcolor", glm::vec3 (0.0f)),
                .skylight = general.optional ("skylightcolor", glm::vec3 (0.0f)),
                .clear = general.user ("clearcolor", properties, glm::vec3 (1.0f)),
            },
            .approxLights = computeApproxLights (
                objects, projection.require <int> ("width", "Projection must have a width"),
                projection.require <int> ("height", "Projection must have a height")
            ),
            .camera = {
                .fade = general.optional ("camerafade", false),
                .preview = general.optional ("camerapreview", false),
                .bloom = {
                    .enabled = general.user ("bloom", properties, false),
                    .strength = general.user ("bloomstrength", properties, 0.0f),
                    .threshold = general.user ("bloomthreshold", properties, 0.0f),
                },
                .parallax = {
                    .enabled = general.user ("cameraparallax", properties, false),
                    .amount = general.user ("cameraparallaxamount", properties, 1.0f),
                    .delay = general.user ("cameraparallaxdelay", properties, 0.0f),
                    .mouseInfluence = general.user ("cameraparallaxmouseinfluence", properties, 1.0f),
                },
                .shake = {
                    .enabled = general.user ("camerashake", properties, false),
                    .amplitude = general.user ("camerashakeamplitude", properties, 0.0f),
                    .roughness = general.user ("camerashakeroughness", properties, 0.0f),
                    .speed = general.user ("camerashakespeed", properties, 0.0f),
                },
                .configuration = {
                    .center = camera.require <glm::vec3> ("center", "Camera must have a center position"),
                    .eye = camera.require <glm::vec3> ("eye", "Camera must have an eye position"),
                    .up = camera.require <glm::vec3> ("up", "Camera must have an up position"),
                },
                .projection = {
                    .width = projection.require <int> ("width", "Projection must have a width"),
                    .height = projection.require <int> ("height", "Projection must have a height"),
                    .isAuto = projection.optional ("auto", false),
                    .nearz = camera.optional <float> ("nearz", 0.0f),
                    .farz = camera.optional <float> ("farz", 1000.0f),
                    .fov = camera.optional <float> ("fov", 50.0f)
                },
                .zoom = general.optional ("zoom", 1.0f)
            },
            .objects = parseObjects (objects, project),
        }
    );
}

VideoUniquePtr WallpaperParser::parseVideo (const JSON& file, Project& project) {
    return std::make_unique<Video> (WallpaperData { .filename = file, .project = project });
}

WebUniquePtr WallpaperParser::parseWeb (const JSON& file, Project& project) {
    return std::make_unique<Web> (WallpaperData {
	.filename = file,
	.project = project,
    });
}

ObjectList WallpaperParser::parseObjects (const JSON& objects, const Project& project) {
    ObjectList result = {};

    for (const auto& cur : objects) {
	result.emplace_back (ObjectParser::parse (cur, project));
    }

    return result;
}