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

		// The scale of aerial perspective's distances, while it is on.
		std::string WithAerialPerspective(const SkyLightComponent& sky)
		{
			if (sky.Source != SkyLightSource::Atmosphere)
				return AtmosphereOnly(sky);
			return sky.AerialPerspective ? std::string() : "Aerial Perspective is off";
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

		// The options shown under another name than the stored one's.
		PropertyOptions Named(PropertyOptions options, std::string displayName)
		{
			options.DisplayName = std::move(displayName);
			return options;
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
		// Point and spot lights' size (highlights only).
		constexpr const char* c_SourceRadiusTooltip = "Radius in meters of the light's bulb or flame: smooth and wet surfaces show its highlight at "
			"that size, the same light spread wider (a lantern about 0.05, a fire about 0.25). Its diffuse light and shadows do not change. 0: a point";

		std::string WithAutoExposure(const PostProcessComponent& post)
		{
			return post.AutoExposure ? std::string() : "Auto Exposure is off";
		}

		const char* GetTonemapperName(TonemapOperator tonemapper)
		{
			switch (tonemapper)
			{
				case TonemapOperator::None: return "None";
				case TonemapOperator::Reinhard: return "Reinhard";
				case TonemapOperator::ACES: return "ACES";
				case TonemapOperator::AgX: return "AgX";
				case TonemapOperator::KhronosNeutral: return "Khronos Neutral";
				case TonemapOperator::Filmic: return "Filmic";
			}
			return "unknown";
		}

		std::string WithFilmicToe(const PostProcessComponent& post)
		{
			return post.Tonemapper == TonemapOperator::Filmic ? std::string()
				: fmt::format("Only the Filmic curve has a toe (Tonemapper is {})", GetTonemapperName(post.Tonemapper));
		}

		std::string WithFilmicShoulder(const PostProcessComponent& post)
		{
			return post.Tonemapper == TonemapOperator::Filmic ? std::string()
				: fmt::format("Only the Filmic curve has a shoulder (Tonemapper is {})", GetTonemapperName(post.Tonemapper));
		}

		std::string WithMotionBlur(const PostProcessComponent& post)
		{
			return post.MotionBlur ? std::string() : "Motion Blur is off";
		}

		std::string WithColorLookup(const PostProcessComponent& post)
		{
			return post.ColorLookup.IsValid() ? std::string() : "No Color Lookup is set";
		}

		std::string WithBloom(const PostProcessComponent& post)
		{
			return post.Bloom ? std::string() : "Bloom is off";
		}

		std::string WithAmbientOcclusion(const PostProcessComponent& post)
		{
			return post.AmbientOcclusion ? std::string() : "Ambient Occlusion is off";
		}

		std::string WithGlobalIllumination(const PostProcessComponent& post)
		{
			return post.GlobalIllumination ? std::string() : "Global Illumination is off";
		}

		std::string WithAntiAliasing(const PostProcessComponent& post)
		{
			return post.AntiAliasing ? std::string() : "Anti Aliasing is off";
		}

		// The name of a method that is not TAA, for the reasons below.
		const char* NonTemporalMethodName(AntiAliasingMode method)
		{
			return method == AntiAliasingMode::Morphological ? "Morphological" : "FXAA";
		}

		// The TAA's look: inactive without anti-aliasing or with another method.
		std::string WithTemporalAntiAliasing(const PostProcessComponent& post)
		{
			if (!post.AntiAliasing)
				return "Anti Aliasing is off";
			return post.AntiAliasingMethod == AntiAliasingMode::TAA ? std::string()
				: fmt::format("Only TAA uses it (Anti Aliasing Method is {})", NonTemporalMethodName(post.AntiAliasingMethod));
		}

		// The screen-space reflections' settings while they are off. Whether they need TAA depends on the renderer's quality
		// (RendererQualitySettings::Reflections: Checkerboard does, Spatial does not), which a component cannot know.
		std::string WithScreenSpaceReflections(const PostProcessComponent& post)
		{
			return post.ScreenSpaceReflections ? std::string() : "Screen Space Reflections is off";
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
			.Section("Light")
			.Property("Color", &DirectionalLightComponent::Color, WithTooltip(Color(), "Linear color of the light, times Intensity: (1, 1, 1) for the "
				"midday sun; the physical atmosphere dims and reddens it toward the horizon itself, so tint it only for a look"))
			.Property("Intensity", &DirectionalLightComponent::Intensity,
				WithTooltip(Range(0.0f, c_Unbounded, 0.05f), "Illuminance in lux on a surface facing the light, times Color: direct sun about 100,000, an "
					"overcast day 1,000 to 10,000, a full moon 0.25"))
			// Stored as ShadowSoftness (scenes, scripts, schemas): it sized only the penumbra before it sized the highlight too.
			.Property("ShadowSoftness", &DirectionalLightComponent::ShadowSoftness, Named(WithTooltip(Range(0.0f, 10.0f, 0.01f), "Angular diameter of "
				"the light's disk in degrees (the sun: about 0.53; an overcast sky's soft sun up to 10): its shadows' penumbra widens with the "
				"distance from the caster, and its highlight on smooth surfaces takes the disk's size. 0: the hardest shadow the filter allows and "
				"a point-like highlight"), "Angular Diameter"))
			.Section("Shadows")
			.Property("CastShadows", &DirectionalLightComponent::CastShadows, WithTooltip({}, "Whether the light casts shadows: cascaded shadow maps out "
				"to Shadow Distance, their resolution and number set by Project Settings > Graphics Quality"))
			.Property("ShadowDistance", &DirectionalLightComponent::ShadowDistance, WithTooltip(Range(1.0f, 10000.0f, 1.0f), "Meters from the camera the "
				"shadows reach; the last cascade fades out before it. Shorter spends the same shadow-map texels on less ground: sharper near shadows"))
			.Property("ShadowBias", &DirectionalLightComponent::ShadowBias, WithTooltip(Range(0.0f, 10.0f, 0.01f), "Depth offset toward the light in "
				"shadow-map texels: raise it against shadow acne (stripes on lit surfaces), lower it when shadows detach from their casters"))
			.Property("ShadowNormalBias", &DirectionalLightComponent::ShadowNormalBias, WithTooltip(Range(0.0f, 10.0f, 0.01f), "Offset of the lookup "
				"along the surface normal in shadow-map texels: raise it against acne on surfaces at grazing angles to the light"))
			.Property("CascadeBlend", &DirectionalLightComponent::CascadeBlend,
				WithTooltip(Slider(0.0f, DirectionalLightComponent::c_MaxCascadeBlend), "Fraction of each shadow cascade's depth range over which it blends into the next one, "
					"hiding the seams between them; the last one fades out by the Shadow Distance. 0 switches at once"))
			.Property("ContactShadowLength", &DirectionalLightComponent::ContactShadowLength,
				WithTooltip(Range(0.0f, DirectionalLightComponent::c_MaxContactShadowLength, 0.01f), "Meters toward the light over which contact "
					"shadows ground objects smaller than the shadow cascades' texels, when Graphics Quality's Contact Shadows has steps (Custom; "
					"no preset draws them); 0 off"))
			.Property("CascadeDistribution", &DirectionalLightComponent::CascadeDistribution,
				WithTooltip(Slider(0.0f, 1.0f), "How the cascades share the distance from 1.5 m to the Shadow Distance: 0 evenly, 1 each "
					"cascade the same ratio farther than the previous. Higher puts more detail near the camera"));
		SetInactiveReasons<DirectionalLightComponent>({ "ShadowDistance", "ShadowBias", "ShadowNormalBias", "CascadeBlend", "ContactShadowLength",
			"CascadeDistribution" }, &WithShadows<DirectionalLightComponent>);

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
			.Property("SourceRadius", &PointLightComponent::SourceRadius,
				WithTooltip(Range(0.0f, PointLightComponent::c_MaxSourceRadius, 0.005f), c_SourceRadiusTooltip))
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
			.Property("SourceRadius", &SpotLightComponent::SourceRadius,
				WithTooltip(Range(0.0f, PointLightComponent::c_MaxSourceRadius, 0.005f), c_SourceRadiusTooltip))
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
				WithTooltip(Range(0.0f, 1.0f, 0.01f), "Atmosphere: diffuse reflectance of the planet's ground, which lights the sky from below"))
			.Property("AerialPerspective", &SkyLightComponent::AerialPerspective,
				WithTooltip({}, "Atmosphere: distant surfaces fade into the air's haze, as the sky does (aerial perspective)"))
			.Property("AerialPerspectiveScale", &SkyLightComponent::AerialPerspectiveScale,
				WithTooltip(Range(0.0f, SkyLightComponent::c_MaxAerialPerspectiveScale, 0.1f), "Atmosphere: distances multiplied by this for the "
					"haze: 1 is Earth's air, 4 makes a 1 km view look like 4 km"));
		SetInactiveReasons<SkyLightComponent>({ "EnvironmentMap", "Rotation" }, &EnvironmentMapOnly);
		SetInactiveReasons<SkyLightComponent>({ "ZenithColor", "HorizonColor", "GroundColor", "SunIntensity" }, &ProceduralSkyOnly);
		SetInactiveReasons<SkyLightComponent>({ "SunSize" }, &WithSunDisk);
		SetInactiveReasons<SkyLightComponent>({ "AtmosphereDensity", "AtmosphereHaze", "GroundAlbedo", "AerialPerspective" }, &AtmosphereOnly);
		SetInactiveReasons<SkyLightComponent>({ "AerialPerspectiveScale" }, &WithAerialPerspective);
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
			.Description("Tonemapping, exposure, bloom, ambient occlusion, anti-aliasing and screen-space reflection settings")
			.Section("Tone and color")
			.EnumProperty("Tonemapper", &PostProcessComponent::Tonemapper, { { "None", 0 }, { "Reinhard", 1 }, { "ACES", 2 }, { "AgX", 3 },
				{ "KhronosNeutral", 4, "Khronos Neutral" }, { "Filmic", 5 } },
				{ .Tooltip = "Filmic: the engine's camera-like curve, middle gray shown as middle gray, a soft shoulder that takes bright colors "
					"to white without changing their hue (Toe and Shoulder shape it); ACES: punchier, darker mid-tones (needs about +0.8 EV for "
					"the same brightness); AgX: flatter and less saturated; Khronos Neutral: keeps base colors up to the highlights; Reinhard; "
					"None: the exposed colors clipped" })
			.Section("Exposure")
			.Property("Exposure", &PostProcessComponent::Exposure, WithTooltip(Range(-16.0f, 16.0f, 0.05f), "Exposure compensation in EV"))
			.Property("AutoExposure", &PostProcessComponent::AutoExposure, WithTooltip({}, "Adapts the exposure to the scene's brightness over time, as "
				"an eye or a camera's meter does, within Auto Exposure Min and Max EV; off, Exposure alone sets it"))
			.Property("AutoExposureMinEV", &PostProcessComponent::AutoExposureMinEV,
				WithTooltip(Range(-12.0f, 16.0f, 0.1f), "Darkest average scene luminance (log2) automatic exposure brightens up to"))
			.Property("AutoExposureMaxEV", &PostProcessComponent::AutoExposureMaxEV,
				WithTooltip(Range(-12.0f, 16.0f, 0.1f), "Brightest average scene luminance (log2) automatic exposure darkens down to"))
			.Property("AutoExposureSpeed", &PostProcessComponent::AutoExposureSpeed,
				WithTooltip(Range(0.0f, 20.0f, 0.05f), "How fast the exposure follows a darker scene (the image brightens), per second; 0 jumps at once"))
			.Section("Bloom")
			.Property("Bloom", &PostProcessComponent::Bloom, WithTooltip({}, "A soft glow around bright light: the light a camera's lens scatters"))
			.Property("BloomIntensity", &PostProcessComponent::BloomIntensity, WithTooltip(Slider(0.0f, 1.0f), "Fraction of the light scattered into the glow"))
			.Property("BloomThreshold", &PostProcessComponent::BloomThreshold,
				WithTooltip(Range(0.0f, 20.0f, 0.05f), "Only light brighter than this (after exposure) blooms; 0 = all light, physically based"))
			.Section("Ambient occlusion")
			.Property("AmbientOcclusion", &PostProcessComponent::AmbientOcclusion, WithTooltip({}, "Darkens creases, corners and contacts that ambient "
				"light reaches less (screen-space ambient occlusion)"))
			.Property("AmbientOcclusionRadius", &PostProcessComponent::AmbientOcclusionRadius, WithTooltip(Range(0.01f, 10.0f, 0.01f),
				"Meters: how far around a surface occluders darken it; on screen kept between 0.3% and 12% of the view's height"))
			.Property("AmbientOcclusionIntensity", &PostProcessComponent::AmbientOcclusionIntensity, WithTooltip(Slider(0.0f, 4.0f), "How strongly the "
				"occlusion darkens ambient light: 1 as measured, 0 none, above 1 stronger"))
			.Property("AmbientOcclusionMultiBounce", &PostProcessComponent::AmbientOcclusionMultiBounce, WithTooltip({},
				"Occluded corners of bright surfaces keep the light their walls reflect into them: white plaster stays light in its creases, "
				"dark soil darkens fully"))
			.Section("Global illumination")
			.Property("GlobalIllumination", &PostProcessComponent::GlobalIllumination, WithTooltip({}, "Experimental. Bounce light and sky "
				"occlusion traced in the world around the camera: sunlit floors light the walls, colored surfaces tint their neighbors, closed "
				"rooms stay dark, whether the source is on screen or not. Graphics Quality High and Ultra; off at Low and Medium"))
			.Property("GlobalIlluminationIntensity", &PostProcessComponent::GlobalIlluminationIntensity, WithTooltip(Range(0.0f, 4.0f, 0.01f),
				"How much of the traced indirect light shades surfaces: 1 as traced"))
			.Section("Anti-aliasing")
			.Property("AntiAliasing", &PostProcessComponent::AntiAliasing, WithTooltip({}, "Smooths jagged edges, by the method below"))
			.EnumProperty("AntiAliasingMethod", &PostProcessComponent::AntiAliasingMethod, { { "FXAA", 0 }, { "TAA", 1 }, { "Morphological", 2 } },
				{ .Tooltip = "FXAA: fast approximate anti-aliasing of each frame; TAA: temporal anti-aliasing, each frame sampled at another "
					"sub-pixel offset and blended with the earlier ones along their motion: smoother, steadier edges and thin details, and "
					"point and spot lights' soft shadow edges without their grain (the sun's are the same with any method). "
					"TAA Preset chooses how the TAA looks. Morphological: non-temporal, finds edges and blends along them, keeping texture "
					"detail" })
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
			.Section("Reflections")
			.Property("ScreenSpaceReflections", &PostProcessComponent::ScreenSpaceReflections,
				WithTooltip({}, "Reflections of what the screen shows, found each frame: smooth floors, water and metal reflect what stands near "
					"them. At Graphics Quality Medium (Reflections Checkerboard) only with TAA; High and Ultra trace every pixel with or without it. "
					"Where they find nothing (off screen, hidden, the sky) the reflection probes and the sky reflect as before"))
			.Property("ScreenSpaceReflectionsIntensity", &PostProcessComponent::ScreenSpaceReflectionsIntensity,
				WithTooltip(Slider(0.0f, 1.0f), "How much the screen-space reflections replace the probes' and the sky's reflection where they "
					"find something; 1: fully"))
			.Property("ScreenSpaceReflectionsMaxRoughness", &PostProcessComponent::ScreenSpaceReflectionsMaxRoughness,
				WithTooltip(Slider(0.0f, 1.0f), "Surfaces rougher than this get none, and they fade out over the last quarter below it: rough "
					"surfaces blur what they reflect, which screen-space reflections do not"))
			.Section("Lens")
			.Property("Vignette", &PostProcessComponent::Vignette,
				WithTooltip(Slider(0.0f, 1.0f), "Natural darkening toward the image's corners, as a lens's (cos^4 of the field angle): 0 none, 1 a "
					"lens's full fall-off; 0.12 darkens the corners of a 60 degree view by about 10%"))
			.Section("Tone and color")
			.Property("Saturation", &PostProcessComponent::Saturation, WithTooltip(Slider(0.0f, 2.0f), "Color saturation after the tone curve: 1 "
				"unchanged, 0 gray, 2 twice as vivid"))
			.Property("Contrast", &PostProcessComponent::Contrast, WithTooltip(Slider(0.5f, 2.0f), "Around middle gray; 1 = unchanged"))
			.Property("Toe", &PostProcessComponent::Toe, WithTooltip(Slider(0.0f, 1.0f), "How deep the shadows go: higher crushes the darkest tones toward "
				"black, lower keeps more detail in them (Filmic only)"))
			.Property("Shoulder", &PostProcessComponent::Shoulder, WithTooltip(Slider(0.0f, 1.0f), "How softly highlights roll off to white: higher "
				"reaches white more stops above middle gray (5 at 0, 6.5 at 0.5, 8 at 1), keeping more highlight detail (Filmic only)"))
			.Property("WhiteBalanceTemperature", &PostProcessComponent::WhiteBalanceTemperature,
				WithTooltip(Range(2000.0f, 15000.0f, 50.0f), "Temperature in kelvin of the light that shows as white: lower values cool the image "
					"(3200 for tungsten light), higher warm it; 6500 is neutral"))
			.Property("WhiteBalanceTint", &PostProcessComponent::WhiteBalanceTint, WithTooltip(Slider(-1.0f, 1.0f), "Tint: positive adds magenta, negative green"))
			.Property("ColorSlope", &PostProcessComponent::ColorSlope, WithTooltip(Range(0.0f, 4.0f, 0.01f), "Slope: per-channel gain before the tone curve"))
			.Property("ColorOffset", &PostProcessComponent::ColorOffset,
				WithTooltip(Range(-0.1f, 0.1f, 0.001f), "Offset: light added per channel, in fractions of middle gray (tints the shadows most)"))
			.Property("ColorPower", &PostProcessComponent::ColorPower, WithTooltip(Range(0.2f, 5.0f, 0.01f), "Power: per-channel contrast around middle gray"))
			.Section("Exposure")
			.EnumProperty("AutoExposureMetering", &PostProcessComponent::AutoExposureMetering, { { "Average", 0 }, { "CenterWeighted", 1, "Center Weighted" } },
				{ .Tooltip = "How the image is metered: Average weighs every pixel alike; CenterWeighted counts the middle of the image most and its "
					"edges a quarter, as a camera's center-weighted meter" })
			.Property("AutoExposureLowPercentile", &PostProcessComponent::AutoExposureLowPercentile,
				WithTooltip(Range(0.0f, 0.49f, 0.01f), "The darkest share of the metered pixels that is ignored, so deep shadows do not move the exposure"))
			.Property("AutoExposureHighPercentile", &PostProcessComponent::AutoExposureHighPercentile,
				WithTooltip(Range(0.51f, 1.0f, 0.01f), "The brightest share of the metered pixels below this is kept: the pixels above it (light "
					"sources) do not move the exposure"))
			.Property("AutoExposureLightSpeed", &PostProcessComponent::AutoExposureLightSpeed,
				WithTooltip(Range(0.0f, 20.0f, 0.05f), "How fast it follows a brighter scene (the image darkens), per second, as eyes leaving a "
					"tunnel; 0 jumps at once"))
			.Property("AutoExposureKey", &PostProcessComponent::AutoExposureKey,
				WithTooltip(Range(0.0f, 0.5f, 0.01f), "How much a scene's brightness against a gray card in the sun shows: 0 meters every scene to "
					"middle gray; 0.12 shows an interior 5 stops below the sun's card 0.6 stops darker (at most 2); scenes above the card are not "
					"brightened. Needs a directional light"))
			.Section("Bloom")
			.Property("BloomRadius", &PostProcessComponent::BloomRadius,
				WithTooltip(Range(0.01f, 0.25f, 0.001f), "How far the glow reaches, as a fraction of the image height: the same share of the image "
					"at every resolution"))
			.Section("Motion")
			.Property("MotionBlur", &PostProcessComponent::MotionBlur,
				WithTooltip({}, "Blurs what moves on screen over the shutter's share of each frame, as a film camera does; editor views never "
					"blur, and players can turn it off"))
			.Property("MotionBlurShutterAngle", &PostProcessComponent::MotionBlurShutterAngle,
				WithTooltip(Range(0.0f, 360.0f, 1.0f), "How long the shutter stays open, in degrees of the frame: 180 blurs over half a frame's "
					"motion, as film cameras do; 0 none"))
			.Property("MotionBlurMaxLength", &PostProcessComponent::MotionBlurMaxLength,
				WithTooltip(Range(0.005f, 0.05f, 0.001f), "The longest blur, as a fraction of the image height: 0.02 is 43 pixels at 2160 rows"))
			.Section("Lens")
			.Property("FilmGrain", &PostProcessComponent::FilmGrain,
				WithTooltip(Slider(0.0f, 1.0f), "Film's grain, new every frame, most visible in the mid-tones; 1 spreads them by about 10 codes. "
					"Players can turn it off"))
			.Property("ChromaticAberration", &PostProcessComponent::ChromaticAberration,
				WithTooltip(Slider(0.0f, 1.0f), "A lens's lateral color fringes, red and blue drawn apart toward the corners; 1 shifts each half a "
					"pixel at a 3840x2160 image's corners. Players can turn it off"))
			.Section("Tone and color")
			.AssetProperty("ColorLookup", &PostProcessComponent::ColorLookup, AssetType::ColorLookup,
				{ .Tooltip = "A grade made in another tool (a .cube file's 3D lookup table), applied to the finished image after the tone curve "
					"and the saturation. Nothing applies while it loads" })
			.Property("ColorLookupIntensity", &PostProcessComponent::ColorLookupIntensity,
				WithTooltip(Slider(0.0f, 1.0f), "How much of the color lookup's grade shows: 1 all of it, 0.5 halfway between the image and the "
					"graded image"))
			.Section("Lens")
			.Property("LensFlare", &PostProcessComponent::LensFlare,
				WithTooltip(Slider(0.0f, 1.0f), "A lens's flares around very bright light, with restraint: faint tinted ghosts mirrored through the "
					"image's center, a halo, and six rays around the sun's disk while it is in view and not covered. 0 none"));
		// Exposure is the compensation SceneRenderer applies with automatic exposure too: always active.
		SetInactiveReasons<PostProcessComponent>({ "AutoExposureMinEV", "AutoExposureMaxEV", "AutoExposureSpeed", "AutoExposureMetering",
			"AutoExposureLowPercentile", "AutoExposureHighPercentile", "AutoExposureLightSpeed", "AutoExposureKey" }, &WithAutoExposure);
		SetInactiveReasons<PostProcessComponent>({ "BloomIntensity", "BloomThreshold", "BloomRadius" }, &WithBloom);
		SetInactiveReasons<PostProcessComponent>({ "MotionBlurShutterAngle", "MotionBlurMaxLength" }, &WithMotionBlur);
		SetInactiveReasons<PostProcessComponent>({ "ColorLookupIntensity" }, &WithColorLookup);
		SetInactiveReasons<PostProcessComponent>({ "Toe" }, &WithFilmicToe);
		SetInactiveReasons<PostProcessComponent>({ "Shoulder" }, &WithFilmicShoulder);
		SetInactiveReasons<PostProcessComponent>({ "AmbientOcclusionRadius", "AmbientOcclusionIntensity", "AmbientOcclusionMultiBounce" },
			&WithAmbientOcclusion);
		SetInactiveReasons<PostProcessComponent>({ "GlobalIlluminationIntensity" }, &WithGlobalIllumination);
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
		SetInactiveReasons<PostProcessComponent>({ "ScreenSpaceReflectionsIntensity", "ScreenSpaceReflectionsMaxRoughness" }, &WithScreenSpaceReflections);

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
