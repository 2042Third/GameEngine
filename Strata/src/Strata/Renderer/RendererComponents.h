#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Strata
{

	// The renderer module's components: lights, the sky, post-processing and text (registered by RegisterRendererComponents,
	// with MeshRendererComponent, whose type is in Scene/Components.h).

	struct DirectionalLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 3.0f;
		bool CastShadows = true;
		float ShadowDistance = 80.0f;   // Shadows are rendered up to this distance from the camera
		float ShadowSoftness = 1.0f;    // Angular diameter of the light in degrees (the sun is about 0.5): penumbra size
		float ShadowBias = 1.0f;        // Depth offset toward the light, in shadow map texels
		float ShadowNormalBias = 1.5f;  // Offset along the surface normal, in shadow map texels
		// Fraction of each shadow cascade's depth range, before its far end, over which it blends into the next cascade (the
		// last one fades out, ending at ShadowDistance): hides the seams between cascades. 0 switches cascades at once.
		float CascadeBlend = 0.1f;

		// At most half of a slice: a cascade's band then never reaches back into the previous one's, so a pixel blends two
		// cascades at most.
		static constexpr float c_MaxCascadeBlend = 0.5f;
	};

	// Point and spot lights cast no shadows (only the directional light does).
	struct PointLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 10.0f;
		float Range = 10.0f; // Light fades to zero at this distance
	};

	// Shines along the entity's forward (-Z) direction.
	struct SpotLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 20.0f;
		float Range = 15.0f;
		float InnerConeAngle = 20.0f; // Degrees, half angle: full intensity inside
		float OuterConeAngle = 30.0f; // Degrees, half angle: no light outside
	};

	enum class SkyLightSource : uint8_t
	{
		EnvironmentMap = 0, // An equirectangular HDR texture
		Procedural          // A sky gradient with the sun's disk, computed from the colors below and the directional light
	};

	// Image-based lighting and sky background, from an HDR environment map (equirectangular texture) or a procedural
	// sky. Both go through the same filtering, so they light the scene the same way.
	struct SkyLightComponent
	{
		AssetHandle EnvironmentMap = UUID::Null();
		float Intensity = 1.0f;
		float Rotation = 0.0f; // Degrees around +Y (environment maps only: the procedural sun follows the light)
		bool ShowBackground = true;
		float BackgroundBlur = 0.0f; // 0 = sharp, 1 = fully blurred
		glm::vec3 AmbientColor = { 0.03f, 0.03f, 0.04f }; // Used when there is no environment (no map assigned)
		SkyLightSource Source = SkyLightSource::EnvironmentMap;
		// Procedural sky: linear radiance straight up, at the horizon and below it. The sun's disk sits where the scene's
		// directional light comes from (45 degrees above the horizon without one), SunSize degrees across, with the light's
		// color times SunIntensity as its radiance.
		glm::vec3 ZenithColor = { 0.18f, 0.32f, 0.62f };
		glm::vec3 HorizonColor = { 0.66f, 0.74f, 0.84f };
		glm::vec3 GroundColor = { 0.22f, 0.20f, 0.18f };
		float SunSize = 0.53f;
		float SunIntensity = 20.0f;
	};

	enum class TonemapOperator : uint8_t
	{
		None = 0,
		Reinhard,
		ACES,
		AgX,
		KhronosNeutral
	};

	// How PostProcessComponent::AntiAliasing smooths edges.
	enum class AntiAliasingMode : uint8_t
	{
		FXAA = 0, // Fast approximate anti-aliasing of the finished image
		// Temporal anti-aliasing: each frame samples its pixels at another sub-pixel offset and is blended with the renderer's
		// history of earlier frames, reprojected along the motion vectors (SceneRendererSpecification::TemporalAntiAliasing)
		TAA
	};

	// Post-processing of the camera image. The first active entity with this component in a scene applies; scenes
	// without one use the defaults.
	struct PostProcessComponent
	{
		TonemapOperator Tonemapper = TonemapOperator::ACES;
		float Exposure = 0.0f; // Exposure compensation in EV (stops); the only exposure without AutoExposure
		// Automatic exposure maps the average luminance of the image's non-black pixels to middle gray. The average is
		// clamped to [2^MinEV, 2^MaxEV] and approached at AutoExposureSpeed per second (0 = instantly).
		bool AutoExposure = true;
		float AutoExposureMinEV = -4.0f;
		float AutoExposureMaxEV = 12.0f;
		float AutoExposureSpeed = 1.5f;
		bool Bloom = true;
		float BloomIntensity = 0.04f; // Fraction of the light scattered into the glow
		float BloomThreshold = 0.0f;  // Only light brighter than this (after exposure) blooms; 0 = all light
		bool AmbientOcclusion = true;
		float AmbientOcclusionRadius = 0.6f; // World units
		float AmbientOcclusionIntensity = 1.0f;
		bool AntiAliasing = true;
		AntiAliasingMode AntiAliasingMethod = AntiAliasingMode::FXAA;
		float Vignette = 0.2f;
		float Saturation = 1.0f;
		float Contrast = 1.0f; // Around middle gray; 1 = unchanged
	};

	enum class TextAlignment : uint8_t
	{
		Left = 0,
		Center,
		Right
	};

	// UTF-8 text, '\n' starts a new line. Screen-space text is a HUD drawn over everything: the anchor point is also the
	// text block's vertical pivot (anchor y 0 hangs the block below the point, 1 stands it above it, 0.5 centers it) and
	// Alignment places each line horizontally (Left starts at the point, Right ends at it). World-space text lies in the
	// entity's XY plane facing +Z, vertically centered on its origin and aligned the same way, hidden behind geometry.
	struct TextComponent
	{
		std::string Text = "Text";
		AssetHandle Font = UUID::Null(); // Default engine font when unset (or while the font loads)
		glm::vec4 Color = { 1.0f, 1.0f, 1.0f, 1.0f }; // Display (sRGB) color; alpha blends
		float FontSize = 32.0f; // Pixels per em in screen space, world units per em in world space
		bool ScreenSpace = true;
		glm::vec2 ScreenAnchor = { 0.5f, 0.5f }; // Normalized viewport position (0,0 = top-left)
		glm::vec2 ScreenOffset = { 0.0f, 0.0f }; // Pixels added to the anchor (+Y down)
		TextAlignment Alignment = TextAlignment::Center;
	};

}
