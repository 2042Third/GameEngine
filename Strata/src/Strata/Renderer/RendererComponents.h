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

	// Light units (DeveloperGuide "Light units and exposure"): the shading computes physical quantities on one scale, so a
	// directional light's intensity is illuminance in lux, a point or spot light's luminous intensity in candela and the
	// sky's and emissive surfaces' radiance luminance in nits (cd/m^2). Nothing rescales them: exposure maps them to the
	// display.
	struct DirectionalLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 3.0f; // Illuminance in lux on a surface facing the light, times Color
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

	// Point and spot lights cast shadows with CastShadows (off by default): a renderer gives the most relevant ones in view
	// (SceneRendererSpecification::MaxShadowedLocalLights) tiles of its shadow atlas, six for a point light, one for a spot light.
	struct PointLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 10.0f; // Luminous intensity in candela, times Color (P lumens in every direction: P / 4pi cd)
		float Range = 10.0f; // Light fades to zero at this distance
		bool CastShadows = false;
		float ShadowBias = 1.0f;         // Depth offset toward the light, in texels of the light's shadow tiles
		float ShadowNormalBias = 1.5f;   // Offset along the surface normal, in texels of the light's shadow tiles
		float ShadowFilterRadius = 1.5f; // Radius of the shadow edge's filter, in texels of the light's shadow tiles (at most 8)

		static constexpr float c_MaxShadowFilterRadius = 8.0f;
	};

	// Shines along the entity's forward (-Z) direction.
	struct SpotLightComponent
	{
		glm::vec3 Color = { 1.0f, 1.0f, 1.0f };
		float Intensity = 20.0f; // Luminous intensity in candela along the axis, times Color (not spread over the cone)
		float Range = 15.0f;
		float InnerConeAngle = 20.0f; // Degrees, half angle: full intensity inside
		float OuterConeAngle = 30.0f; // Degrees, half angle: no light outside
		bool CastShadows = false;
		float ShadowBias = 1.0f;         // As PointLightComponent's
		float ShadowNormalBias = 1.5f;
		float ShadowFilterRadius = 1.5f; // At most PointLightComponent::c_MaxShadowFilterRadius
	};

	enum class SkyLightSource : uint8_t
	{
		EnvironmentMap = 0, // An equirectangular HDR texture
		Procedural,         // A sky gradient with the sun's disk, computed from the colors below and the directional light
		// Earth's atmosphere lit by the directional light: the sky its air and haze scatter, the sun's disk, and the light's
		// color at the ground (Renderer/Atmosphere.h)
		Atmosphere
	};

	// Image-based lighting and sky background, from an HDR environment map (equirectangular texture), a procedural sky or
	// a physical atmosphere. All of them go through the same filtering, so they light the scene the same way.
	struct SkyLightComponent
	{
		AssetHandle EnvironmentMap = UUID::Null();
		float Intensity = 1.0f; // Multiplies the sky's luminance: the map's values and the colors below are in nits
		float Rotation = 0.0f; // Degrees around +Y (environment maps only: the procedural sun follows the light)
		bool ShowBackground = true;
		float BackgroundBlur = 0.0f; // 0 = sharp, 1 = fully blurred
		glm::vec3 AmbientColor = { 0.03f, 0.03f, 0.04f }; // Luminance in nits, used when there is no environment (no map)
		SkyLightSource Source = SkyLightSource::EnvironmentMap;
		// Procedural sky: linear luminance in nits straight up, at the horizon and below it. The sun's disk sits where the
		// scene's directional light comes from (45 degrees above the horizon without one), SunSize degrees across, with the
		// light's color times SunIntensity as its luminance.
		glm::vec3 ZenithColor = { 0.18f, 0.32f, 0.62f };
		glm::vec3 HorizonColor = { 0.66f, 0.74f, 0.84f };
		glm::vec3 GroundColor = { 0.22f, 0.20f, 0.18f };
		float SunSize = 0.53f; // Degrees across (the procedural sky's and the atmosphere's disk)
		float SunIntensity = 20.0f;
		// Atmosphere: the directional light is the sun above the atmosphere (its Intensity the illuminance there, about
		// 120,000 lux for the real sun), dimmed and reddened on its way to the ground; the sky is the light its air and
		// aerosols scatter, in nits per lux of sun times Intensity. AtmosphereDensity scales the air and the aerosols (1:
		// Earth's), AtmosphereHaze the aerosols once more (haze, the whitish glow around the sun), GroundAlbedo is the
		// planet's ground's diffuse reflectance (the light it sends back up into the sky).
		float AtmosphereDensity = 1.0f;
		float AtmosphereHaze = 1.0f;
		float GroundAlbedo = 0.3f;
	};

	// Exponential height fog over the scene's surfaces and its sky. The first active entity with this component in a scene
	// applies. Its density is Density at BaseHeight and falls by the factor e every 1 / HeightFalloff meters above it (rises
	// below it); the renderer integrates it along each view ray from StartDistance on, per pixel (no volume: no light
	// shafts or local lights in the fog).
	struct HeightFogComponent
	{
		float Density = 0.02f;      // Extinction at BaseHeight, per meter
		float HeightFalloff = 0.2f; // Per meter: how fast the density falls with height
		float BaseHeight = 0.0f;    // World height (meters) where the density is Density
		float StartDistance = 0.0f; // Meters from the camera before which there is no fog
		float MaxOpacity = 1.0f;    // The most the fog hides of what lies behind it (the sky at the horizon gets this much)
		glm::vec3 Color = { 0.7f, 0.8f, 0.9f }; // Linear albedo of the scattering: what fraction of its light the fog scatters
		// The sky lights the fog: the procedural sky's horizon color (so fog that hides the distance meets the sky without a
		// seam), an environment map's irradiance from above, or the ambient color without an environment; off, Color is the
		// fog's own radiance. The directional light lights it in both cases (SunScattering).
		bool ColorFromSky = true;
		// How strongly the fog scatters the directional light forward, toward a viewer looking at the light: 0 scatters it
		// equally in every direction, 1 gathers it into a glow around the light (the Henyey-Greenstein phase function's
		// asymmetry is c_MaxSunAsymmetry times this).
		float SunScattering = 0.5f;

		// Below 1, so the glow around the light stays finite.
		static constexpr float c_MaxSunAsymmetry = 0.9f;
		// Above 0: no falloff would make the density the same at every height, whose integral toward the sky never ends.
		static constexpr float c_MinHeightFalloff = 0.0001f;
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
		// clamped to [2^MinEV, 2^MaxEV] and approached at AutoExposureSpeed per second (0 = instantly). MaxEV reaches what
		// a scene lit by a real sun meters (around 15); scenes saved with another value keep it.
		bool AutoExposure = true;
		float AutoExposureMinEV = -4.0f;
		float AutoExposureMaxEV = 16.0f;
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
