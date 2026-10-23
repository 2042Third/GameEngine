#include "stpch.h"
#include "Strata/Renderer/RendererComponents.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyOptions.h"
#include "Strata/Renderer/RendererRegistration.h"
#include "Strata/Scene/Components.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string>

namespace Strata
{

	namespace
	{

		// Inactive reasons (PropertyInfo::InactiveReason) of the lights', the sky's and post-processing's properties, each
		// following the code that reads the property in SceneRenderer: shadows, the sky and its environment, the ambient light,
		// automatic exposure, bloom and ambient occlusion.
		template<typename T>
		void SetInactiveReasons(std::initializer_list<const char*> properties, std::string (*reason)(const T&))
		{
			for (const char* property : properties)
				ComponentRegistry::SetInactiveReason<T>(property, reason);
		}

		const char* GetSourceName(SkyLightSource source)
		{
			switch (source)
			{
				case SkyLightSource::EnvironmentMap: return "Environment Map";
				case SkyLightSource::Procedural: return "Procedural";
				case SkyLightSource::Atmosphere: return "Atmosphere";
			}
			return "unknown";
		}

		std::string ProceduralSkyOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::Procedural ? std::string() : fmt::format("Only the procedural sky uses it (Source is {})", GetSourceName(sky.Source));
		}

		std::string EnvironmentMapOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::EnvironmentMap ? std::string() : fmt::format("Only an environment map uses it (Source is {})", GetSourceName(sky.Source));
		}

		std::string AtmosphereOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::Atmosphere ? std::string() : fmt::format("Only the atmosphere uses it (Source is {})", GetSourceName(sky.Source));
		}

		// The sun's disk: the procedural sky's and the atmosphere's.
		std::string WithSunDisk(const SkyLightComponent& sky)
		{
			return sky.Source != SkyLightSource::EnvironmentMap ? std::string() : "Only the procedural sky and the atmosphere draw a sun (Source is Environment Map)";
		}

		// The atmosphere's background is drawn from its sky-view table, never blurred.
		std::string WithoutAtmosphere(const SkyLightComponent& sky)
		{
			return sky.Source != SkyLightSource::Atmosphere ? std::string() : "The atmosphere's background is always sharp (Source is Atmosphere)";
		}

		// The renderer lights with the environment once there is one: the procedural sky, the atmosphere, or a map that is set.
		std::string WithoutEnvironmentOnly(const SkyLightComponent& sky)
		{
			if (sky.Source == SkyLightSource::Procedural)
				return "The procedural sky lights the scene: the ambient color applies only without an environment";
			if (sky.Source == SkyLightSource::Atmosphere)
				return "The atmosphere lights the scene: the ambient color applies only without an environment";
			if (sky.EnvironmentMap.IsValid())
				return "The environment map lights the scene: the ambient color applies only without one";
			return {};
		}

		template<typename Light>
		std::string WithShadows(const Light& light)
		{
			return light.CastShadows ? std::string() : "Cast Shadows is off";
		}

		// The shadow settings of point and spot lights (their units: texels of the light's shadow tiles, which follow how large
		// the light looks on screen).
		constexpr const char* c_LocalShadowBiasTooltip = "Depth offset toward the light, in texels of the light's shadow tiles: raise it if lit "
			"surfaces show stripes of shadow (acne)";
		constexpr const char* c_LocalShadowNormalBiasTooltip = "Offset along the surface normal, in texels of the light's shadow tiles: raise it "
			"against acne on surfaces lit at grazing angles; too much detaches shadows from their casters";
		constexpr const char* c_LocalShadowFilterTooltip = "Radius of the shadow's soft edge, in texels of the light's shadow tiles (0 to 8)";
		constexpr const char* c_LocalShadowCastTooltip = "Casts shadows: the renderer gives the most relevant shadowed lights in view (16) tiles "
			"of its shadow atlas, six for a point light, one for a spot light; each costs drawing its casters again when they move";

		std::string WithAutoExposure(const PostProcessComponent& post)
		{
			return post.AutoExposure ? std::string() : "Auto Exposure is off";
		}

		std::string WithBloom(const PostProcessComponent& post)
		{
			return post.Bloom ? std::string() : "Bloom is off";
		}

		std::string WithAmbientOcclusion(const PostProcessComponent& post)
		{
			return post.AmbientOcclusion ? std::string() : "Ambient Occlusion is off";
		}

		std::string WithAntiAliasing(const PostProcessComponent& post)
		{
			return post.AntiAliasing ? std::string() : "Anti Aliasing is off";
		}

		// The TAA's look: inactive without anti-aliasing or with FXAA.
		std::string WithTemporalAntiAliasing(const PostProcessComponent& post)
		{
			if (!post.AntiAliasing)
				return "Anti Aliasing is off";
			return post.AntiAliasingMethod == AntiAliasingMode::TAA ? std::string() : "Only TAA uses it (Anti Aliasing Method is FXAA)";
		}

		const char* GetPresetName(TemporalAntiAliasingPreset preset)
		{
			return preset == TemporalAntiAliasingPreset::Sharp ? "Sharp" : "Original";
		}

		// A TAA tunable: inactive as the look is, and unless Custom, where the reason quotes the value the preset uses (the
		// row shows the stored one). `presetValue` formats a table's field, or returns nothing where the preset does not use it
		// (`unused` then says why).
		template<typename Value>
		std::string WithCustomTemporalAntiAliasing(const PostProcessComponent& post, Value (*presetValue)(const TemporalAntiAliasingSettings&),
			std::string (*unused)(TemporalAntiAliasingPreset))
		{
			std::string reason = WithTemporalAntiAliasing(post);
			if (!reason.empty() || post.TAAPreset == TemporalAntiAliasingPreset::Custom)
				return reason;
			const TemporalAntiAliasingSettings& table = post.TAAPreset == TemporalAntiAliasingPreset::Sharp ? c_TAASharpSettings : c_TAAOriginalSettings;
			if (unused)
			{
				reason = unused(post.TAAPreset);
				if (!reason.empty())
					return reason;
			}
			return fmt::format("The {} preset sets it ({}): choose Custom to tune it", GetPresetName(post.TAAPreset), presetValue(table));
		}

		// The tunables' reasons, one function per property (SetInactiveReason takes a function of the component).
		template<typename Value, Value TemporalAntiAliasingSettings::*Field>
		std::string WithCustomTunable(const PostProcessComponent& post)
		{
			return WithCustomTemporalAntiAliasing<Value>(post, [](const TemporalAntiAliasingSettings& settings) { return settings.*Field; }, nullptr);
		}

		std::string WithCustomBool(const PostProcessComponent& post)
		{
			return WithCustomTemporalAntiAliasing<const char*>(post,
				[](const TemporalAntiAliasingSettings& settings) { return settings.DisocclusionRejection ? "on" : "off"; }, nullptr);
		}

		std::string WithCustomClamp(const PostProcessComponent& post)
		{
			return WithCustomTemporalAntiAliasing<const char*>(post,
				[](const TemporalAntiAliasingSettings& settings) { return settings.HistoryClamp == TemporalHistoryClamp::Variance ? "Tight" : "Range"; }, nullptr);
		}

		// Read only by tight clipping.
		std::string WithTightClipping(const PostProcessComponent& post)
		{
			std::string reason = WithCustomTemporalAntiAliasing<float>(post, [](const TemporalAntiAliasingSettings& settings) { return settings.ClipWidth; },
				[](TemporalAntiAliasingPreset preset)
				{
					return preset == TemporalAntiAliasingPreset::Original ? std::string("The Original preset clamps to the colors' range: choose Custom to tune it")
																		 : std::string();
				});
			if (reason.empty() && post.TAAHistoryClamp != TemporalHistoryClamp::Variance)
				reason = "Only Tight clipping uses it (TAA History Clamp is Range)";
			return reason;
		}

		// Read only with disocclusion rejection.
		std::string WithDisocclusionRejection(const PostProcessComponent& post)
		{
			std::string reason = WithCustomTemporalAntiAliasing<float>(post, [](const TemporalAntiAliasingSettings& settings) { return settings.DisocclusionThreshold; },
				[](TemporalAntiAliasingPreset preset)
				{
					return preset == TemporalAntiAliasingPreset::Original ? std::string("The Original preset does not reject uncovered surfaces: choose Custom to tune it")
																		 : std::string();
				});
			if (reason.empty() && !post.TAADisocclusionRejection)
				reason = "TAA Disocclusion Rejection is off";
			return reason;
		}

	}

	void RegisterRendererComponents()
	{
		ComponentRegistry::Register<MeshRendererComponent>("MeshRenderer")
			.DisplayName("Mesh Renderer")
			.Category("Rendering")
			.AssetProperty("Mesh", &MeshRendererComponent::Mesh, AssetType::Mesh)
			.AssetProperty("Material", &MeshRendererComponent::Material, AssetType::Material, { .Tooltip = "Overrides every submesh material when set" })
			.Property("CastShadows", &MeshRendererComponent::CastShadows);

		ComponentRegistry::Register<DirectionalLightComponent>("DirectionalLight")
			.DisplayName("Directional Light")
			.Category("Lighting")
			.Description("Sun-like light shining along the entity's forward (-Z) direction; its intensity is illuminance in lux")
			.Property("Color", &DirectionalLightComponent::Color, Color())
			.Property("Intensity", &DirectionalLightComponent::Intensity,
				WithTooltip(Range(0.0f, c_Unbounded, 0.05f), "Illuminance in lux on a surface facing the light, times Color: direct sun about 100,000, an "
					"overcast day 1,000 to 10,000, a full moon 0.25"))
			.Property("CastShadows", &DirectionalLightComponent::CastShadows)
			.Property("ShadowDistance", &DirectionalLightComponent::ShadowDistance, Range(1.0f, 10000.0f, 1.0f))
			.Property("ShadowSoftness", &DirectionalLightComponent::ShadowSoftness, Range(0.0f, 10.0f, 0.01f))
			.Property("ShadowBias", &DirectionalLightComponent::ShadowBias, Range(0.0f, 10.0f, 0.01f))
			.Property("ShadowNormalBias", &DirectionalLightComponent::ShadowNormalBias, Range(0.0f, 10.0f, 0.01f))
			.Property("CascadeBlend", &DirectionalLightComponent::CascadeBlend,
				WithTooltip(Slider(0.0f, DirectionalLightComponent::c_MaxCascadeBlend), "Fraction of each shadow cascade's depth range over which it blends into the next one, "
					"hiding the seams between them; the last one fades out by the Shadow Distance. 0 switches at once"));
		SetInactiveReasons<DirectionalLightComponent>({ "ShadowDistance", "ShadowSoftness", "ShadowBias", "ShadowNormalBias", "CascadeBlend" },
			&WithShadows<DirectionalLightComponent>);

		ComponentRegistry::Register<PointLightComponent>("PointLight")
			.DisplayName("Point Light")
			.Category("Lighting")
			.Description("Light shining in every direction from the entity's position, with shadows in six directions when it casts them; its "
				"intensity is luminous intensity in candela")
			.Property("Color", &PointLightComponent::Color, Color())
			.Property("Intensity", &PointLightComponent::Intensity,
				WithTooltip(Range(0.0f, c_Unbounded, 0.1f), "Luminous intensity in candela, times Color, falling off with the square of the distance: a "
					"bulb of P lumens has P / 12.57 cd (800 lm, about 64 cd), a candle about 1"))
			.Property("Range", &PointLightComponent::Range, WithTooltip(Range(0.01f, c_Unbounded, 0.1f), "Light fades to zero at this distance"))
			.Property("CastShadows", &PointLightComponent::CastShadows, WithTooltip({}, c_LocalShadowCastTooltip))
			.Property("ShadowBias", &PointLightComponent::ShadowBias, WithTooltip(Range(0.0f, 10.0f, 0.01f), c_LocalShadowBiasTooltip))
			.Property("ShadowNormalBias", &PointLightComponent::ShadowNormalBias, WithTooltip(Range(0.0f, 10.0f, 0.01f), c_LocalShadowNormalBiasTooltip))
			.Property("ShadowFilterRadius", &PointLightComponent::ShadowFilterRadius,
				WithTooltip(Range(0.0f, PointLightComponent::c_MaxShadowFilterRadius, 0.01f), c_LocalShadowFilterTooltip));
		SetInactiveReasons<PointLightComponent>({ "ShadowBias", "ShadowNormalBias", "ShadowFilterRadius" }, &WithShadows<PointLightComponent>);

		ComponentRegistry::Register<SpotLightComponent>("SpotLight")
			.DisplayName("Spot Light")
			.Category("Lighting")
			.Description("Cone light shining along the entity's forward (-Z) direction, with shadows when it casts them; its intensity is "
				"luminous intensity in candela along its axis")
			.Property("Color", &SpotLightComponent::Color, Color())
			.Property("Intensity", &SpotLightComponent::Intensity,
				WithTooltip(Range(0.0f, c_Unbounded, 0.1f), "Luminous intensity in candela along the axis, times Color, falling off with the square of "
					"the distance; a narrower cone does not make it brighter"))
			.Property("Range", &SpotLightComponent::Range, Range(0.01f, c_Unbounded, 0.1f))
			.Property("InnerConeAngle", &SpotLightComponent::InnerConeAngle, WithTooltip(Range(0.0f, 89.0f, 0.1f), "Half angle in degrees"))
			.Property("OuterConeAngle", &SpotLightComponent::OuterConeAngle, WithTooltip(Range(0.1f, 89.9f, 0.1f), "Half angle in degrees"))
			.Property("CastShadows", &SpotLightComponent::CastShadows, WithTooltip({}, c_LocalShadowCastTooltip))
			.Property("ShadowBias", &SpotLightComponent::ShadowBias, WithTooltip(Range(0.0f, 10.0f, 0.01f), c_LocalShadowBiasTooltip))
			.Property("ShadowNormalBias", &SpotLightComponent::ShadowNormalBias, WithTooltip(Range(0.0f, 10.0f, 0.01f), c_LocalShadowNormalBiasTooltip))
			.Property("ShadowFilterRadius", &SpotLightComponent::ShadowFilterRadius,
				WithTooltip(Range(0.0f, PointLightComponent::c_MaxShadowFilterRadius, 0.01f), c_LocalShadowFilterTooltip));
		SetInactiveReasons<SpotLightComponent>({ "ShadowBias", "ShadowNormalBias", "ShadowFilterRadius" }, &WithShadows<SpotLightComponent>);

		ComponentRegistry::Register<SkyLightComponent>("SkyLight")
			.DisplayName("Sky Light")
			.Category("Lighting")
			.Description("Image-based lighting and sky background from an HDR environment map, a procedural sky or a physical atmosphere lit by the "
				"directional light; its radiance is luminance in nits (cd/m\xC2\xB2) times its intensity (the atmosphere's: per lux of the light's "
				"illuminance)")
			.EnumProperty("Source", &SkyLightComponent::Source, { { "EnvironmentMap", 0, "Environment Map" }, { "Procedural", 1 }, { "Atmosphere", 2 } },
				{ .Tooltip = "EnvironmentMap: an HDR texture; Procedural: a sky computed from the colors below and the directional light; Atmosphere: "
					"Earth's sky lit by the directional light, which it dims and reddens toward the horizon" })
			.AssetProperty("EnvironmentMap", &SkyLightComponent::EnvironmentMap, AssetType::Texture, { .Tooltip = "Equirectangular HDR texture" })
			.Property("Intensity", &SkyLightComponent::Intensity,
				WithTooltip(Range(0.0f, c_Unbounded, 0.01f), "Multiplies the sky's luminance in nits (cd/m\xC2\xB2): an environment map's values, the "
					"procedural colors, the ambient color, and the atmosphere's sky (nits per lux of the directional light)"))
			.Property("Rotation", &SkyLightComponent::Rotation, WithTooltip(Range(-360.0f, 360.0f, 0.5f), "Degrees around +Y (environment maps only)"))
			.Property("ShowBackground", &SkyLightComponent::ShowBackground)
			.Property("BackgroundBlur", &SkyLightComponent::BackgroundBlur, Slider(0.0f, 1.0f))
			.Property("AmbientColor", &SkyLightComponent::AmbientColor,
				WithTooltip(Color(), "Ambient sky luminance in nits (linear, times Intensity) while there is no environment (no map set)"))
			.Property("ZenithColor", &SkyLightComponent::ZenithColor,
				WithTooltip(Color(), "Procedural sky: luminance straight up in nits (linear, times Intensity)"))
			.Property("HorizonColor", &SkyLightComponent::HorizonColor,
				WithTooltip(Color(), "Procedural sky: luminance at the horizon in nits (linear, times Intensity)"))
			.Property("GroundColor", &SkyLightComponent::GroundColor,
				WithTooltip(Color(), "Procedural sky: luminance below the horizon in nits (linear, times Intensity)"))
			.Property("SunSize", &SkyLightComponent::SunSize,
				WithTooltip(Range(0.0f, 20.0f, 0.01f), "Procedural sky and atmosphere: angular diameter of the sun's disk in degrees (0 hides it)"))
			.Property("SunIntensity", &SkyLightComponent::SunIntensity,
				WithTooltip(Range(0.0f, 60000.0f, 0.1f), "Procedural sky: luminance of the sun's disk in nits, times the directional light's color (not its "
					"intensity) and this sky's Intensity"))
			.Property("AtmosphereDensity", &SkyLightComponent::AtmosphereDensity,
				WithTooltip(Range(0.0f, 10.0f, 0.01f), "Atmosphere: density of its air and aerosols (1: Earth's); thicker air reddens the sun sooner"))
			.Property("AtmosphereHaze", &SkyLightComponent::AtmosphereHaze,
				WithTooltip(Range(0.0f, 10.0f, 0.01f), "Atmosphere: amount of aerosols (1: a clear day), the whitish haze and glow around the sun"))
			.Property("GroundAlbedo", &SkyLightComponent::GroundAlbedo,
				WithTooltip(Range(0.0f, 1.0f, 0.01f), "Atmosphere: diffuse reflectance of the planet's ground, which lights the sky from below"));
		SetInactiveReasons<SkyLightComponent>({ "EnvironmentMap", "Rotation" }, &EnvironmentMapOnly);
		SetInactiveReasons<SkyLightComponent>({ "ZenithColor", "HorizonColor", "GroundColor", "SunIntensity" }, &ProceduralSkyOnly);
		SetInactiveReasons<SkyLightComponent>({ "SunSize" }, &WithSunDisk);
		SetInactiveReasons<SkyLightComponent>({ "AtmosphereDensity", "AtmosphereHaze", "GroundAlbedo" }, &AtmosphereOnly);
		SetInactiveReasons<SkyLightComponent>({ "BackgroundBlur" }, &WithoutAtmosphere);
		SetInactiveReasons<SkyLightComponent>({ "AmbientColor" }, &WithoutEnvironmentOnly);

		ComponentRegistry::Register<HeightFogComponent>("HeightFog")
			.DisplayName("Height Fog")
			.Category("Rendering")
			.Description("Fog that thickens toward the ground, over the scene's surfaces and its sky (the first active one applies)")
			.Property("Density", &HeightFogComponent::Density,
				WithTooltip(Range(0.0f, c_Unbounded, 0.001f), "Extinction per meter at the base height: 0.02 hides half of what lies 35 m away"))
			.Property("HeightFalloff", &HeightFogComponent::HeightFalloff,
				WithTooltip(Range(HeightFogComponent::c_MinHeightFalloff, c_Unbounded, 0.001f),
					"Per meter: the density falls by the factor e every 1 / falloff meters above the base height and rises below it"))
			.Property("BaseHeight", &HeightFogComponent::BaseHeight, WithTooltip(Range(-c_Unbounded, c_Unbounded, 0.1f), "World height in meters where the density is Density"))
			.Property("StartDistance", &HeightFogComponent::StartDistance, WithTooltip(Range(0.0f, c_Unbounded, 0.1f), "Meters from the camera before which there is no fog"))
			.Property("MaxOpacity", &HeightFogComponent::MaxOpacity,
				WithTooltip(Slider(0.0f, 1.0f), "The most the fog hides of what lies behind it; the sky at the horizon gets this much"))
			.Property("Color", &HeightFogComponent::Color, WithTooltip(Color(), "Linear albedo: the fraction of its light the fog scatters toward the viewer"))
			.Property("ColorFromSky", &HeightFogComponent::ColorFromSky,
				WithTooltip({}, "The sky lights the fog: the procedural sky's horizon color, an environment map's light from above, or the ambient color "
					"without either; off, Color is the fog's own radiance. The directional light lights it either way"))
			.Property("SunScattering", &HeightFogComponent::SunScattering,
				WithTooltip(Slider(0.0f, 1.0f), "How strongly the fog scatters the directional light toward a viewer looking at it: 0 equally in every "
					"direction, 1 into a glow around the light"));

		ComponentRegistry::Register<ReflectionProbeComponent>("ReflectionProbe")
			.DisplayName("Reflection Probe")
			.Category("Rendering")
			.Description("Reflections and ambient light inside a box from a capture of the scene around the entity (lighting.bakeProbes renders "
				"it); where boxes overlap the two strongest blend")
			.AssetProperty("Capture", &ReflectionProbeComponent::Capture, AssetType::Texture,
				{ .Tooltip = "Equirectangular HDR texture of the scene seen from the probe (luminance in nits): Bake writes one" })
			.Property("Extents", &ReflectionProbeComponent::Extents,
				WithTooltip(Range(ReflectionProbeComponent::c_MinExtent, c_Unbounded, 0.05f), "Half size of the influence box in meters, along the "
					"entity's axes (times its scale)"))
			.Property("BlendDistance", &ReflectionProbeComponent::BlendDistance,
				WithTooltip(Range(0.0f, c_Unbounded, 0.05f), "Meters inside the box's faces over which the probe fades into the sky's lighting "
					"(or a weaker probe's)"))
			.Property("BoxProjection", &ReflectionProbeComponent::BoxProjection,
				WithTooltip({}, "Reflections are projected onto the box's faces, so walls near the box's faces reflect where they are (rooms); "
					"off, the capture is seen as if infinitely far (open spaces)"))
			.Property("Intensity", &ReflectionProbeComponent::Intensity, WithTooltip(Range(0.0f, c_Unbounded, 0.01f), "Multiplies the capture's luminance"))
			.Property("Priority", &ReflectionProbeComponent::Priority,
				WithTooltip(Range(-1000.0f, 1000.0f, 0.1f), "Where boxes overlap the higher priority wins (then the smaller box); with more probes in "
					"view than the renderer keeps, the strongest stay"));

		ComponentRegistry::Register<PostProcessComponent>("PostProcess")
			.DisplayName("Post Process")
			.Category("Rendering")
			.Description("Tonemapping, exposure, bloom, ambient occlusion and anti-aliasing settings")
			.EnumProperty("Tonemapper", &PostProcessComponent::Tonemapper, { { "None", 0 }, { "Reinhard", 1 }, { "ACES", 2 }, { "AgX", 3 },
				{ "KhronosNeutral", 4, "Khronos Neutral" } })
			.Property("Exposure", &PostProcessComponent::Exposure, WithTooltip(Range(-16.0f, 16.0f, 0.05f), "Exposure compensation in EV"))
			.Property("AutoExposure", &PostProcessComponent::AutoExposure)
			.Property("AutoExposureMinEV", &PostProcessComponent::AutoExposureMinEV,
				WithTooltip(Range(-12.0f, 16.0f, 0.1f), "Darkest average scene luminance (log2) automatic exposure brightens up to"))
			.Property("AutoExposureMaxEV", &PostProcessComponent::AutoExposureMaxEV,
				WithTooltip(Range(-12.0f, 16.0f, 0.1f), "Brightest average scene luminance (log2) automatic exposure darkens down to"))
			.Property("AutoExposureSpeed", &PostProcessComponent::AutoExposureSpeed, WithTooltip(Range(0.0f, 20.0f, 0.05f), "Adaptation rate per second; 0 = instant"))
			.Property("Bloom", &PostProcessComponent::Bloom)
			.Property("BloomIntensity", &PostProcessComponent::BloomIntensity, WithTooltip(Slider(0.0f, 1.0f), "Fraction of the light scattered into the glow"))
			.Property("BloomThreshold", &PostProcessComponent::BloomThreshold,
				WithTooltip(Range(0.0f, 20.0f, 0.05f), "Only light brighter than this (after exposure) blooms; 0 = all light, physically based"))
			.Property("AmbientOcclusion", &PostProcessComponent::AmbientOcclusion)
			.Property("AmbientOcclusionRadius", &PostProcessComponent::AmbientOcclusionRadius, WithTooltip(Range(0.01f, 10.0f, 0.01f), "World units"))
			.Property("AmbientOcclusionIntensity", &PostProcessComponent::AmbientOcclusionIntensity, Slider(0.0f, 4.0f))
			.Property("AntiAliasing", &PostProcessComponent::AntiAliasing, WithTooltip({}, "Smooths jagged edges, by the method below"))
			.EnumProperty("AntiAliasingMethod", &PostProcessComponent::AntiAliasingMethod, { { "FXAA", 0 }, { "TAA", 1 } },
				{ .Tooltip = "FXAA: fast approximate anti-aliasing of each frame; TAA: temporal anti-aliasing, each frame sampled at another "
					"sub-pixel offset and blended with the earlier ones along their motion: smoother edges, shadows and ambient occlusion. "
					"TAA Preset chooses how the TAA looks" })
			.EnumProperty("TAAPreset", &PostProcessComponent::TAAPreset, { { "Sharp", 0 }, { "Original", 1 }, { "Custom", 2 } },
				{ .Tooltip = "How the TAA looks. Sharp: tighter history clipping, slow pans resampled less, uncovered surfaces drop their "
					"history, transparent surfaces take mostly the current frame, a faster response to motion and a light sharpening of the "
					"displayed image. Original: the first TAA, softer and steadier. Custom: the tunables below (choosing it in the "
					"Inspector starts from the look in effect)" })
			.Property("TAAHistoryFrames", &PostProcessComponent::TAAHistoryFrames,
				WithTooltip(Range(static_cast<float>(c_TAAMinHistoryFrames), static_cast<float>(c_TAAMaxHistoryFrames), 1.0f),
					"About how many frames the image averages: the current frame weighs at least 1 / this. At 8, every jitter position counts "
					"once. More is smoother and steadier but lags behind lighting changes; fewer reacts faster but edges crawl"))
			.Property("TAAMotionResponse", &PostProcessComponent::TAAMotionResponse,
				WithTooltip(Range(0.0f, c_TAAMaxMotionResponse, 0.001f), "How quickly moving surfaces let go of the history. At 0.0625, motion of "
					"8 pixels a frame weighs the current frame 0.5. Higher keeps moving things crisper and their trails shorter, but they get "
					"less anti-aliasing while they move. 0: motion does not matter"))
			.Property("TAAMaxMotionWeight", &PostProcessComponent::TAAMaxMotionWeight,
				WithTooltip(Slider(0.0f, 1.0f), "The most the current frame weighs, however fast things move. 1 lets very fast motion drop the "
					"history: sharp, but aliased"))
			.Property("TAAResampleWeight", &PostProcessComponent::TAAResampleWeight,
				WithTooltip(Slider(0.0f, 1.0f), "The current frame's least weight where the history is read between pixels, as in slow pans; it is "
					"largest halfway between them. Higher keeps slowly moving texture crisper, but its edges get less anti-aliasing while they "
					"move. 0: off"))
			.EnumProperty("TAAHistoryClamp", &PostProcessComponent::TAAHistoryClamp, { { "MinMax", 0, "Range" }, { "Variance", 1, "Tight (variance)" } },
				{ .Tooltip = "How the history is kept consistent with what the pixel shows now. Range limits it to the range of the colors "
					"around the pixel. Tight limits it to a narrower box around their average, which removes more ghosting and trails, most "
					"where the surroundings are smooth and less beside glints" })
			.Property("TAAClipWidth", &PostProcessComponent::TAAClipWidth,
				WithTooltip(Range(c_TAAMinClipWidth, c_TAAMaxClipWidth, 0.05f), "Tight only: the box's half width, in standard deviations of the "
					"colors around the pixel. Lower removes more trails, but lets edges flicker and lets noise through"))
			.Property("TAATransparentWeight", &PostProcessComponent::TAATransparentWeight,
				WithTooltip(Slider(0.0f, 1.0f), "The current frame's least weight where a blended transparent surface covers the pixel (half "
					"cover or more counts as full). Transparent surfaces have no motion vectors of their own. 0 treats them like everything "
					"else, so they ghost when they move; 1 drops their history, so they never ghost, but their edges shimmer"))
			.Property("TAADisocclusionRejection", &PostProcessComponent::TAADisocclusionRejection,
				WithTooltip({}, "Drop the history where a surface was just uncovered (something nearer covered it in the previous frame), so "
					"moving objects leave no ghost on what they uncover"))
			.Property("TAADisocclusionThreshold", &PostProcessComponent::TAADisocclusionThreshold,
				WithTooltip(Range(c_TAAMinDisocclusionThreshold, c_TAAMaxDisocclusionThreshold, 0.005f), "How much nearer the previous frame's "
					"surface must have been, as a fraction of the distance, for the pixel to count as uncovered. Lower catches thin gaps, such "
					"as a hand in front of a wall, but may drop the history of surfaces moving fast away from the camera"))
			.Property("TAASharpening", &PostProcessComponent::TAASharpening,
				WithTooltip(Slider(0.0f, 1.0f), "Sharpens the displayed image after TAA, never the history, to restore texture detail the "
					"averaging softens. Strong edges are sharpened much less and overshoot is bounded, so halos stay faint. 0 is off"))
			.Property("Vignette", &PostProcessComponent::Vignette, Slider(0.0f, 1.0f))
			.Property("Saturation", &PostProcessComponent::Saturation, Slider(0.0f, 2.0f))
			.Property("Contrast", &PostProcessComponent::Contrast, WithTooltip(Slider(0.5f, 2.0f), "Around middle gray; 1 = unchanged"));
		// Exposure is the compensation SceneRenderer applies with automatic exposure too: always active.
		SetInactiveReasons<PostProcessComponent>({ "AutoExposureMinEV", "AutoExposureMaxEV", "AutoExposureSpeed" }, &WithAutoExposure);
		SetInactiveReasons<PostProcessComponent>({ "BloomIntensity", "BloomThreshold" }, &WithBloom);
		SetInactiveReasons<PostProcessComponent>({ "AmbientOcclusionRadius", "AmbientOcclusionIntensity" }, &WithAmbientOcclusion);
		SetInactiveReasons<PostProcessComponent>({ "AntiAliasingMethod" }, &WithAntiAliasing);
		SetInactiveReasons<PostProcessComponent>({ "TAAPreset" }, &WithTemporalAntiAliasing);
		SetInactiveReasons<PostProcessComponent>({ "TAAHistoryFrames" }, &WithCustomTunable<uint32_t, &TemporalAntiAliasingSettings::HistoryFrames>);
		SetInactiveReasons<PostProcessComponent>({ "TAAMotionResponse" }, &WithCustomTunable<float, &TemporalAntiAliasingSettings::MotionResponse>);
		SetInactiveReasons<PostProcessComponent>({ "TAAMaxMotionWeight" }, &WithCustomTunable<float, &TemporalAntiAliasingSettings::MaxMotionWeight>);
		SetInactiveReasons<PostProcessComponent>({ "TAAResampleWeight" }, &WithCustomTunable<float, &TemporalAntiAliasingSettings::ResampleWeight>);
		SetInactiveReasons<PostProcessComponent>({ "TAAHistoryClamp" }, &WithCustomClamp);
		SetInactiveReasons<PostProcessComponent>({ "TAAClipWidth" }, &WithTightClipping);
		SetInactiveReasons<PostProcessComponent>({ "TAATransparentWeight" }, &WithCustomTunable<float, &TemporalAntiAliasingSettings::TransparentWeight>);
		SetInactiveReasons<PostProcessComponent>({ "TAADisocclusionRejection" }, &WithCustomBool);
		SetInactiveReasons<PostProcessComponent>({ "TAADisocclusionThreshold" }, &WithDisocclusionRejection);
		SetInactiveReasons<PostProcessComponent>({ "TAASharpening" }, &WithCustomTunable<float, &TemporalAntiAliasingSettings::Sharpening>);

		ComponentRegistry::Register<TextComponent>("Text")
			.Category("UI")
			.Description("Text drawn in screen space (HUD) or in the world")
			.Property("Text", &TextComponent::Text, { .Flags = PropertyFlags::MultiLine })
			.AssetProperty("Font", &TextComponent::Font, AssetType::Font)
			.Property("Color", &TextComponent::Color, Color())
			.Property("FontSize", &TextComponent::FontSize, Range(1.0f, 1024.0f, 0.5f))
			.Property("ScreenSpace", &TextComponent::ScreenSpace)
			.Property("ScreenAnchor", &TextComponent::ScreenAnchor, WithTooltip(Range(0.0f, 1.0f, 0.005f), "Normalized viewport position; (0,0) is the top-left corner"))
			.Property("ScreenOffset", &TextComponent::ScreenOffset, WithTooltip({}, "Pixel offset from the anchor (+Y down)"))
			.EnumProperty("Alignment", &TextComponent::Alignment, { { "Left", 0 }, { "Center", 1 }, { "Right", 2 } });
	}

	namespace
	{

		// A tunable within its range; NaN (which no comparison orders) becomes the range's low end.
		float ClampTunable(float value, float low, float high)
		{
			return value >= low ? std::min(value, high) : low;
		}

		// The component's tunables as stored, unclamped.
		TemporalAntiAliasingSettings GetStoredTemporalAntiAliasingSettings(const PostProcessComponent& postProcess)
		{
			TemporalAntiAliasingSettings settings;
			settings.HistoryFrames = postProcess.TAAHistoryFrames;
			settings.MotionResponse = postProcess.TAAMotionResponse;
			settings.MaxMotionWeight = postProcess.TAAMaxMotionWeight;
			settings.ResampleWeight = postProcess.TAAResampleWeight;
			settings.HistoryClamp = postProcess.TAAHistoryClamp;
			settings.ClipWidth = postProcess.TAAClipWidth;
			settings.TransparentWeight = postProcess.TAATransparentWeight;
			settings.DisocclusionRejection = postProcess.TAADisocclusionRejection;
			settings.DisocclusionThreshold = postProcess.TAADisocclusionThreshold;
			settings.Sharpening = postProcess.TAASharpening;
			return settings;
		}

		// Clamped to the ranges, with 0 in the fields the chosen modes do not read.
		TemporalAntiAliasingSettings NormalizeTemporalAntiAliasingSettings(TemporalAntiAliasingSettings settings)
		{
			settings.HistoryFrames = std::clamp(settings.HistoryFrames, c_TAAMinHistoryFrames, c_TAAMaxHistoryFrames);
			settings.MotionResponse = ClampTunable(settings.MotionResponse, 0.0f, c_TAAMaxMotionResponse);
			settings.MaxMotionWeight = ClampTunable(settings.MaxMotionWeight, 0.0f, 1.0f);
			settings.ResampleWeight = ClampTunable(settings.ResampleWeight, 0.0f, 1.0f);
			if (settings.HistoryClamp != TemporalHistoryClamp::Variance)
				settings.HistoryClamp = TemporalHistoryClamp::MinMax;
			settings.ClipWidth = settings.HistoryClamp == TemporalHistoryClamp::Variance ? ClampTunable(settings.ClipWidth, c_TAAMinClipWidth, c_TAAMaxClipWidth) : 0.0f;
			settings.TransparentWeight = ClampTunable(settings.TransparentWeight, 0.0f, 1.0f);
			settings.DisocclusionThreshold = settings.DisocclusionRejection
				? ClampTunable(settings.DisocclusionThreshold, c_TAAMinDisocclusionThreshold, c_TAAMaxDisocclusionThreshold) : 0.0f;
			settings.Sharpening = ClampTunable(settings.Sharpening, 0.0f, 1.0f);
			return settings;
		}

	}

	TemporalAntiAliasingSettings GetTemporalAntiAliasingSettings(const PostProcessComponent& postProcess)
	{
		switch (postProcess.TAAPreset)
		{
			case TemporalAntiAliasingPreset::Sharp: return c_TAASharpSettings;
			case TemporalAntiAliasingPreset::Original: return c_TAAOriginalSettings;
			case TemporalAntiAliasingPreset::Custom: break;
		}
		return NormalizeTemporalAntiAliasingSettings(GetStoredTemporalAntiAliasingSettings(postProcess));
	}

	TemporalAntiAliasingSettings GetTemporalAntiAliasingCustomStart(const PostProcessComponent& postProcess)
	{
		const TemporalAntiAliasingSettings stored = NormalizeTemporalAntiAliasingSettings(GetStoredTemporalAntiAliasingSettings(postProcess));
		// Tunables that hold a preset's table were not tuned (they are the defaults, or a preset's start): Custom starts from
		// the look in effect. Anything else is someone's tuning, kept.
		if (stored == c_TAAOriginalSettings || stored == c_TAASharpSettings)
			return GetTemporalAntiAliasingSettings(postProcess);
		return GetStoredTemporalAntiAliasingSettings(postProcess);
	}

	void SetTemporalAntiAliasingSettings(PostProcessComponent& postProcess, const TemporalAntiAliasingSettings& settings)
	{
		postProcess.TAAHistoryFrames = settings.HistoryFrames;
		postProcess.TAAMotionResponse = settings.MotionResponse;
		postProcess.TAAMaxMotionWeight = settings.MaxMotionWeight;
		postProcess.TAAResampleWeight = settings.ResampleWeight;
		postProcess.TAAHistoryClamp = settings.HistoryClamp;
		if (settings.HistoryClamp == TemporalHistoryClamp::Variance)
			postProcess.TAAClipWidth = settings.ClipWidth;
		postProcess.TAATransparentWeight = settings.TransparentWeight;
		postProcess.TAADisocclusionRejection = settings.DisocclusionRejection;
		if (settings.DisocclusionRejection)
			postProcess.TAADisocclusionThreshold = settings.DisocclusionThreshold;
		postProcess.TAASharpening = settings.Sharpening;
	}

}
