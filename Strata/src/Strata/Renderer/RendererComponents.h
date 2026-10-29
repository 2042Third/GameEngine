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
		float ShadowSoftness = 0.53f;   // Angular diameter of the light in degrees (the sun: 0.53, the default): penumbra size
		float ShadowBias = 1.0f;        // Depth offset toward the light, in shadow map texels
		float ShadowNormalBias = 1.5f;  // Offset along the surface normal, in shadow map texels
		// Fraction of each shadow cascade's depth range, before its far end, over which it blends into the next cascade (the
		// last one fades out, ending at ShadowDistance): hides the seams between cascades. 0 switches cascades at once.
		float CascadeBlend = 0.1f;
		// How far toward the light, in meters, the screen-space contact shadows look for what the cascades are too coarse to show
		// (at High and Ultra: RendererQualitySettings::ContactShadows); 0 turns them off for this light.
		float ContactShadowLength = 0.3f;
		// How the shadow cascades share the distances from the nearest to the farthest surface the camera sees, once they are
		// fitted to them (SceneRendererSpecification::FitShadowsToView): 0 evenly, 1 each cascade the same ratio farther than the
		// previous one. Higher puts more of the shadows' detail near the camera.
		float CascadeDistribution = 0.9f;

		// At most half of a slice: a cascade's band then never reaches back into the previous one's, so a pixel blends two
		// cascades at most.
		static constexpr float c_MaxCascadeBlend = 0.5f;
		// Contact shadows march at most this far (meters): beyond, the cascades show what the march could.
		static constexpr float c_MaxContactShadowLength = 2.0f;
	};

	// Point and spot lights cast shadows with CastShadows (off by default): a renderer gives the most relevant ones in view
	// (RendererQualitySettings::MaxShadowedLocalLights) tiles of its shadow atlas, six for a point light, one for a spot light.
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
		// The defaults give a clear day's sun to sky ratio on flat ground (about 8 with the default 3 lux sun 45 degrees up, as the
		// atmosphere's): Strata 0.6.0's colors times 0.25.
		glm::vec3 ZenithColor = { 0.045f, 0.08f, 0.155f };
		glm::vec3 HorizonColor = { 0.165f, 0.185f, 0.21f };
		glm::vec3 GroundColor = { 0.055f, 0.05f, 0.045f };
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
		// Atmosphere: the air between the camera and every surface dims it and adds the light it scatters toward the camera, as
		// the sky does over its whole depth (aerial perspective), distances multiplied by AerialPerspectiveScale: worlds smaller
		// than real ones (an island of 1 km) show the haze of larger ones above 1.
		bool AerialPerspective = true;
		float AerialPerspectiveScale = 1.0f; // 0 to c_MaxAerialPerspectiveScale
		static constexpr float c_MaxAerialPerspectiveScale = 64.0f;
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

	// Reflections and ambient light inside a box from a capture of the scene seen from the entity's position: an
	// equirectangular HDR texture (luminance in nits) that lighting.bakeProbes renders and imports, or any such texture. The
	// box follows the entity's position, rotation and scale; inside it, surfaces reflect the capture (projected onto the box's
	// faces with BoxProjection, as the walls of a room are) and are lit by its irradiance, fading toward the sky's lighting
	// over BlendDistance before the box's faces. Where boxes overlap, the two strongest blend: higher Priority first, then the
	// smaller box. The renderer keeps the strongest probes in view (SceneRendererSpecification::MaxReflectionProbes).
	struct ReflectionProbeComponent
	{
		AssetHandle Capture = 0;                    // Equirectangular HDR texture of the scene around the probe (nits)
		glm::vec3 Extents = { 5.0f, 5.0f, 5.0f };   // Half extents of the influence box in meters, along the entity's axes
		float BlendDistance = 1.0f;                 // Meters inside the box's faces over which the probe fades out
		bool BoxProjection = true;                  // Reflections hit the box's faces (rooms, courtyards) rather than infinity
		float Intensity = 1.0f;                     // Multiplies the capture's luminance
		int32_t Priority = 0;                       // Higher wins where boxes overlap (then the smaller box)

		static constexpr float c_MinExtent = 0.01f; // Meters: a box of no size would reflect nothing
	};

	// The tone curve of PostProcessComponent. The values are script and file API: they never change; new curves are appended.
	enum class TonemapOperator : uint8_t
	{
		None = 0,
		Reinhard,
		ACES,
		AgX,
		KhronosNeutral,
		// The engine's camera-like curve (Include/Filmic.glsl, Tools/FitFilmic.py): middle gray shown as middle gray, a soft
		// shoulder that takes bright colors to white without changing their hue; shaped by PostProcessComponent::Toe, Shoulder
		Filmic
	};

	// How automatic exposure weighs the image's pixels (PostProcessComponent::AutoExposureMetering). The values are script and
	// file API: they never change.
	enum class ExposureMetering : uint8_t
	{
		Average = 0,       // Every pixel alike
		CenterWeighted = 1 // The middle of the image counts most and its edges a quarter, as a camera's center-weighted meter
	};

	// How PostProcessComponent::AntiAliasing smooths edges.
	enum class AntiAliasingMode : uint8_t
	{
		FXAA = 0, // Fast approximate anti-aliasing of the finished image
		// Temporal anti-aliasing: each frame samples its pixels at another sub-pixel offset and is blended with the renderer's
		// history of earlier frames, reprojected along the motion vectors (SceneRendererSpecification::TemporalAntiAliasing)
		TAA,
		// Morphological anti-aliasing of each frame on its own: finds edges, reconstructs the lines their staircases stand for
		// and blends across them by the areas the lines cover (nothing temporal)
		Morphological
	};

	// How the TAA looks (PostProcessComponent::TAAPreset). The values are script and file API: they never change.
	enum class TemporalAntiAliasingPreset : uint8_t
	{
		Sharp = 0,    // c_TAASharpSettings: tighter clipping, less resampling, reactive transparents, rejection, sharpening
		Original = 1, // c_TAAOriginalSettings: the softer, steadier first TAA
		Custom = 2    // The component's TAA tunables
	};

	// How the TAA keeps the history consistent with what the pixel shows now (TemporalAntiAliasingSettings::HistoryClamp).
	// The values are script and file API: they never change.
	enum class TemporalHistoryClamp : uint8_t
	{
		MinMax = 0,  // Clamped to the per-component range of the neighborhood's colors
		Variance = 1 // Clipped toward the neighborhood's mean, into a box ClipWidth standard deviations wide (within the range)
	};

	// What the TAA resolves with (GetTemporalAntiAliasingSettings): a preset's table, or a component's tunables under
	// Custom. Fields the chosen modes do not read are 0 in a normalized value.
	struct TemporalAntiAliasingSettings
	{
		uint32_t HistoryFrames = 8;      // The current frame weighs at least 1 / this
		float MotionResponse = 0.0f;     // The current frame's weight per pixel of motion...
		float MaxMotionWeight = 0.0f;    // ...up to this
		float ResampleWeight = 0.0f;     // Its least weight where the history is read halfway between texels; 0 = off
		TemporalHistoryClamp HistoryClamp = TemporalHistoryClamp::MinMax;
		float ClipWidth = 0.0f;          // Variance only: the box's half width, in standard deviations
		float TransparentWeight = 0.0f;  // Its least weight under blended transparent surfaces; 0 = off
		bool DisocclusionRejection = false;
		float DisocclusionThreshold = 0.0f; // With rejection: how much nearer, as a fraction of the distance, counts as covered
		float Sharpening = 0.0f;         // Of the displayed image only, never the history; 0 = off

		bool operator==(const TemporalAntiAliasingSettings&) const = default;
	};

	// The tunables' ranges (the properties' and those GetTemporalAntiAliasingSettings clamps Custom's values to). Weights
	// and the sharpening lie in [0, 1].
	inline constexpr uint32_t c_TAAMinHistoryFrames = 2;
	inline constexpr uint32_t c_TAAMaxHistoryFrames = 32;
	inline constexpr float c_TAAMaxMotionResponse = 0.25f;
	inline constexpr float c_TAAMinClipWidth = 0.5f;
	inline constexpr float c_TAAMaxClipWidth = 4.0f;
	inline constexpr float c_TAAMinDisocclusionThreshold = 0.01f;
	inline constexpr float c_TAAMaxDisocclusionThreshold = 0.5f;

	// The presets' tables (normalized). Original is the first TAA: the current frame weighs at least 1/8 and 1/32 more per
	// pixel of motion up to 1/2, the history is clamped to the neighborhood's range, and nothing else applies.
	inline constexpr TemporalAntiAliasingSettings c_TAAOriginalSettings = { .HistoryFrames = 8, .MotionResponse = 1.0f / 32.0f, .MaxMotionWeight = 0.5f,
		.ResampleWeight = 0.0f, .HistoryClamp = TemporalHistoryClamp::MinMax, .ClipWidth = 0.0f, .TransparentWeight = 0.0f, .DisocclusionRejection = false,
		.DisocclusionThreshold = 0.0f, .Sharpening = 0.0f };
	// Sharp keeps the least weight at 1/8, so captures settle after the same eight frames as under Original.
	inline constexpr TemporalAntiAliasingSettings c_TAASharpSettings = { .HistoryFrames = 8, .MotionResponse = 0.0625f, .MaxMotionWeight = 0.5f,
		.ResampleWeight = 0.25f, .HistoryClamp = TemporalHistoryClamp::Variance, .ClipWidth = 1.25f, .TransparentWeight = 0.5f, .DisocclusionRejection = true,
		.DisocclusionThreshold = 0.05f, .Sharpening = 0.5f };

	// Post-processing of the camera image. The first active entity with this component in a scene applies; scenes
	// without one use the defaults.
	struct PostProcessComponent
	{
		// Filmic for new components and scenes without one (ACES until 0.7.0: files store the curve, so their scenes keep it).
		TonemapOperator Tonemapper = TonemapOperator::Filmic;
		float Exposure = 0.0f; // Exposure compensation in EV (stops); the only exposure without AutoExposure
		// Automatic exposure maps the average luminance of the image's non-black pixels (between the metering window's
		// percentiles, weighed by AutoExposureMetering) to middle gray, offset by the exposure key. The average is clamped to
		// [2^MinEV, 2^MaxEV] and approached at AutoExposureSpeed per second toward darker scenes and AutoExposureLightSpeed
		// toward brighter ones (0 = instantly). MaxEV reaches what a scene lit by a real sun meters (around 15); scenes saved
		// with another value keep it.
		bool AutoExposure = true;
		float AutoExposureMinEV = -4.0f;
		float AutoExposureMaxEV = 16.0f;
		float AutoExposureSpeed = 1.5f;
		bool Bloom = true;
		float BloomIntensity = 0.04f; // Fraction of the light scattered into the glow
		float BloomThreshold = 0.0f;  // Only light brighter than this (after exposure) blooms; 0 = all light
		// How far the glow reaches, as a fraction of the image's height (its deepest level's texel): the same share of the image
		// at every resolution. 64 / 1080 by default: 1080 rows use 0.6.0's six levels exactly, 2160 rows seven.
		float BloomRadius = 64.0f / 1080.0f;
		// Motion blur (off by default): what moves on screen blurs over the shutter's share of each frame (MotionBlurShutterAngle
		// degrees of 360: 180 blurs over half a frame's motion), clamped to MotionBlurMaxLength of the image's height. Editor views
		// never blur, and players can turn it off (RendererQualitySettings::MotionBlur).
		bool MotionBlur = false;
		float MotionBlurShutterAngle = 180.0f;
		float MotionBlurMaxLength = 0.02f;
		bool AmbientOcclusion = true;
		float AmbientOcclusionRadius = 1.0f; // Meters; on screen kept between 0.3% and 12% of the view's height
		float AmbientOcclusionIntensity = 1.0f;
		// Occluded corners of bright surfaces keep the light their walls reflect into them (the forward pass's AOMultiBounce): also
		// for materials' occlusion maps, with or without AmbientOcclusion.
		bool AmbientOcclusionMultiBounce = true;
		bool AntiAliasing = true;
		AntiAliasingMode AntiAliasingMethod = AntiAliasingMode::FXAA;
		// The TAA's look: a preset, or the tunables below under Custom. The preset and the tunables are independent: choosing
		// a preset writes no tunable (GetTemporalAntiAliasingCustomStart is where Custom starts from in the Inspector). Files
		// without it (every scene saved before the presets) load as Sharp.
		TemporalAntiAliasingPreset TAAPreset = TemporalAntiAliasingPreset::Sharp;
		uint32_t TAAHistoryFrames = c_TAASharpSettings.HistoryFrames;
		float TAAMotionResponse = c_TAASharpSettings.MotionResponse;
		float TAAMaxMotionWeight = c_TAASharpSettings.MaxMotionWeight;
		float TAAResampleWeight = c_TAASharpSettings.ResampleWeight;
		TemporalHistoryClamp TAAHistoryClamp = c_TAASharpSettings.HistoryClamp;
		float TAAClipWidth = c_TAASharpSettings.ClipWidth;
		float TAATransparentWeight = c_TAASharpSettings.TransparentWeight;
		bool TAADisocclusionRejection = c_TAASharpSettings.DisocclusionRejection;
		float TAADisocclusionThreshold = c_TAASharpSettings.DisocclusionThreshold;
		float TAASharpening = c_TAASharpSettings.Sharpening;
		// Screen-space reflections, with TAA only (they read its history and leave their noise to it): what the previous frame
		// showed where the view's reflection meets the depth buffer, over the probes' and the sky's specular light.
		bool ScreenSpaceReflections = false;
		float ScreenSpaceReflectionsIntensity = 1.0f;    // How much they replace the probes' and the sky's reflection, 0 to 1
		float ScreenSpaceReflectionsMaxRoughness = 0.5f; // Rougher surfaces reflect nothing; the last quarter below it fades
		// A lens's natural darkening toward the corners, applied before the tone curve: the share of cos^4 of the field angle
		// (1: a lens's full fall-off). 0.12 darkens a 60 degree view's corners by about 10% (0.2 until 0.7.0, a ring after the
		// curve: files keep their value under the lens's model).
		float Vignette = 0.12f;
		// A lens's and film's flaws, with restraint (off by default; players can turn each off): grain in the mid-tones that
		// changes every frame (FilmGrain 1: about 10 codes of spread), and lateral chromatic aberration, red and blue fringes
		// toward the corners (ChromaticAberration 1: half a pixel at a 3840x2160 image's corners).
		float FilmGrain = 0.0f;
		float ChromaticAberration = 0.0f;
		// A grade made in another tool (a ColorLookup asset, imported from a .cube file), applied after the tone curve and the
		// saturation to the display-encoded image, mixed in by ColorLookupIntensity. Nothing applies while it loads.
		AssetHandle ColorLookup = UUID::Null();
		float ColorLookupIntensity = 1.0f;
		// A lens's flares around very bright light (0: none): ghosts mirrored through the image's center, a halo and the
		// directional light's starburst when its disk is in view. The bloom chain feeds them (it runs for them with Bloom off).
		float LensFlare = 0.0f;
		float Saturation = 1.0f;
		float Contrast = 1.0f; // Around middle gray; 1 = unchanged
		// The Filmic curve's look, each in [0, 1]: how deep the shadows go (higher crushes the darkest tones) and how softly the
		// highlights roll off to white (higher reaches white more stops above middle gray: 5 at 0, 6.5 at 0.5, 8 at 1).
		float Toe = 0.5f;
		float Shoulder = 0.5f;
		// Color, on scene-linear light before the tone curve (Saturation is after it): the white balance names the light that
		// shows as white, by its temperature in kelvin (2,000 to 15,000; 6500 is neutral) and a tint along green-magenta (-1 to
		// 1, positive adds magenta); then a per-channel slope (gain), offset (light added, in fractions of middle gray) and
		// power around middle gray. The defaults change nothing.
		float WhiteBalanceTemperature = 6500.0f;
		float WhiteBalanceTint = 0.0f;
		glm::vec3 ColorSlope = glm::vec3(1.0f);
		glm::vec3 ColorOffset = glm::vec3(0.0f);
		glm::vec3 ColorPower = glm::vec3(1.0f);
		// Automatic exposure's metering: the pixels' weights, and the darkest and brightest shares of them ignored (so deep
		// shadows and light sources do not move the exposure; HighPercentile - LowPercentile is at least 0.02).
		ExposureMetering AutoExposureMetering = ExposureMetering::CenterWeighted;
		float AutoExposureLowPercentile = 0.1f;
		float AutoExposureHighPercentile = 0.9f;
		float AutoExposureLightSpeed = 3.0f; // Per second, toward brighter scenes (eyes leaving a tunnel); 0 = instantly
		// The exposure key: the metered average's stops below the luminance of a gray card facing the scene's directional light,
		// times this, darken the image by up to 2 stops, so interiors read darker than sunlight as far as their light is darker;
		// a scene above the card is not brightened (RenderData::c_ExposureKeyMaxStops). 0 meters every scene to middle gray;
		// without a directional light there is no key.
		float AutoExposureKey = 0.12f;
	};

	// The settings the TAA resolves with: the preset's table, or the component's tunables under Custom, clamped to their
	// ranges and normalized (0 in ClipWidth under MinMax and in DisocclusionThreshold without rejection), so equal looks
	// compare equal whatever the unused fields hold.
	TemporalAntiAliasingSettings GetTemporalAntiAliasingSettings(const PostProcessComponent& postProcess);
	// The values Custom starts from when chosen in the Inspector: the table of the preset in effect while the stored tunables
	// (normalized) equal one of the two tables, otherwise the stored tunables, which are then someone's tuning.
	TemporalAntiAliasingSettings GetTemporalAntiAliasingCustomStart(const PostProcessComponent& postProcess);
	// Writes the settings into the component's tunables; a field the settings normalized away (ClipWidth under MinMax,
	// DisocclusionThreshold without rejection) keeps its stored value.
	void SetTemporalAntiAliasingSettings(PostProcessComponent& postProcess, const TemporalAntiAliasingSettings& settings);

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
