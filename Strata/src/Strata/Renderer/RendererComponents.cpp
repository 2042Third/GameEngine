#include "stpch.h"
#include "Strata/Renderer/RendererComponents.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyOptions.h"
#include "Strata/Renderer/RendererRegistration.h"
#include "Strata/Scene/Components.h"

#include <string>

namespace Strata
{

	namespace
	{

		// Inactive reasons (PropertyInfo::InactiveReason) of the sky's and post-processing's properties, each following the code
		// that reads the property in SceneRenderer: the sky and its environment, the ambient light, automatic exposure, bloom
		// and ambient occlusion.
		template<typename T>
		void SetInactiveReasons(std::initializer_list<const char*> properties, std::string (*reason)(const T&))
		{
			for (const char* property : properties)
				ComponentRegistry::SetInactiveReason<T>(property, reason);
		}

		std::string ProceduralSkyOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::Procedural ? std::string() : "Only the procedural sky uses it (Source is Environment Map)";
		}

		std::string EnvironmentMapOnly(const SkyLightComponent& sky)
		{
			return sky.Source == SkyLightSource::EnvironmentMap ? std::string() : "Only an environment map uses it (Source is Procedural)";
		}

		// The renderer lights with the environment once there is one: the procedural sky, or a map that is set.
		std::string WithoutEnvironmentOnly(const SkyLightComponent& sky)
		{
			if (sky.Source == SkyLightSource::Procedural)
				return "The procedural sky lights the scene: the ambient color applies only without an environment";
			if (sky.EnvironmentMap.IsValid())
				return "The environment map lights the scene: the ambient color applies only without one";
			return {};
		}

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
			.Description("Sun-like light shining along the entity's forward (-Z) direction")
			.Property("Color", &DirectionalLightComponent::Color, Color())
			.Property("Intensity", &DirectionalLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.05f))
			.Property("CastShadows", &DirectionalLightComponent::CastShadows)
			.Property("ShadowDistance", &DirectionalLightComponent::ShadowDistance, Range(1.0f, 10000.0f, 1.0f))
			.Property("ShadowSoftness", &DirectionalLightComponent::ShadowSoftness, Range(0.0f, 10.0f, 0.01f))
			.Property("ShadowBias", &DirectionalLightComponent::ShadowBias, Range(0.0f, 10.0f, 0.01f))
			.Property("ShadowNormalBias", &DirectionalLightComponent::ShadowNormalBias, Range(0.0f, 10.0f, 0.01f));

		ComponentRegistry::Register<PointLightComponent>("PointLight")
			.DisplayName("Point Light")
			.Category("Lighting")
			.Description("Light shining in every direction from the entity's position (casts no shadows)")
			.Property("Color", &PointLightComponent::Color, Color())
			.Property("Intensity", &PointLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.1f))
			.Property("Range", &PointLightComponent::Range, WithTooltip(Range(0.01f, c_Unbounded, 0.1f), "Light fades to zero at this distance"));

		ComponentRegistry::Register<SpotLightComponent>("SpotLight")
			.DisplayName("Spot Light")
			.Category("Lighting")
			.Description("Cone light shining along the entity's forward (-Z) direction (casts no shadows)")
			.Property("Color", &SpotLightComponent::Color, Color())
			.Property("Intensity", &SpotLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.1f))
			.Property("Range", &SpotLightComponent::Range, Range(0.01f, c_Unbounded, 0.1f))
			.Property("InnerConeAngle", &SpotLightComponent::InnerConeAngle, WithTooltip(Range(0.0f, 89.0f, 0.1f), "Half angle in degrees"))
			.Property("OuterConeAngle", &SpotLightComponent::OuterConeAngle, WithTooltip(Range(0.1f, 89.9f, 0.1f), "Half angle in degrees"));

		ComponentRegistry::Register<SkyLightComponent>("SkyLight")
			.DisplayName("Sky Light")
			.Category("Lighting")
			.Description("Image-based lighting and sky background from an HDR environment map or a procedural sky")
			.EnumProperty("Source", &SkyLightComponent::Source, { { "EnvironmentMap", 0, "Environment Map" }, { "Procedural", 1 } },
				{ .Tooltip = "EnvironmentMap: an HDR texture; Procedural: a sky computed from the colors below and the directional light" })
			.AssetProperty("EnvironmentMap", &SkyLightComponent::EnvironmentMap, AssetType::Texture, { .Tooltip = "Equirectangular HDR texture" })
			.Property("Intensity", &SkyLightComponent::Intensity, Range(0.0f, c_Unbounded, 0.01f))
			.Property("Rotation", &SkyLightComponent::Rotation, WithTooltip(Range(-360.0f, 360.0f, 0.5f), "Degrees around +Y (environment maps only)"))
			.Property("ShowBackground", &SkyLightComponent::ShowBackground)
			.Property("BackgroundBlur", &SkyLightComponent::BackgroundBlur, Slider(0.0f, 1.0f))
			.Property("AmbientColor", &SkyLightComponent::AmbientColor, WithTooltip(Color(), "Ambient light while there is no environment (no map set)"))
			.Property("ZenithColor", &SkyLightComponent::ZenithColor, WithTooltip(Color(), "Procedural sky: color straight up (linear radiance)"))
			.Property("HorizonColor", &SkyLightComponent::HorizonColor, WithTooltip(Color(), "Procedural sky: color at the horizon (linear radiance)"))
			.Property("GroundColor", &SkyLightComponent::GroundColor, WithTooltip(Color(), "Procedural sky: color below the horizon (linear radiance)"))
			.Property("SunSize", &SkyLightComponent::SunSize, WithTooltip(Range(0.0f, 20.0f, 0.01f), "Procedural sky: angular diameter of the sun's disk in degrees (0 hides it)"))
			.Property("SunIntensity", &SkyLightComponent::SunIntensity,
				WithTooltip(Range(0.0f, 60000.0f, 0.1f), "Procedural sky: radiance of the sun's disk, times the directional light's color"));
		SetInactiveReasons<SkyLightComponent>({ "EnvironmentMap", "Rotation" }, &EnvironmentMapOnly);
		SetInactiveReasons<SkyLightComponent>({ "ZenithColor", "HorizonColor", "GroundColor", "SunSize", "SunIntensity" }, &ProceduralSkyOnly);
		SetInactiveReasons<SkyLightComponent>({ "AmbientColor" }, &WithoutEnvironmentOnly);

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
					"sub-pixel offset and blended with the earlier ones along their motion: smoother edges, shadows and ambient occlusion, "
					"a slight blur of what moves" })
			.Property("Vignette", &PostProcessComponent::Vignette, Slider(0.0f, 1.0f))
			.Property("Saturation", &PostProcessComponent::Saturation, Slider(0.0f, 2.0f))
			.Property("Contrast", &PostProcessComponent::Contrast, WithTooltip(Slider(0.5f, 2.0f), "Around middle gray; 1 = unchanged"));
		// Exposure is the compensation SceneRenderer applies with automatic exposure too: always active.
		SetInactiveReasons<PostProcessComponent>({ "AutoExposureMinEV", "AutoExposureMaxEV", "AutoExposureSpeed" }, &WithAutoExposure);
		SetInactiveReasons<PostProcessComponent>({ "BloomIntensity", "BloomThreshold" }, &WithBloom);
		SetInactiveReasons<PostProcessComponent>({ "AmbientOcclusionRadius", "AmbientOcclusionIntensity" }, &WithAmbientOcclusion);
		SetInactiveReasons<PostProcessComponent>({ "AntiAliasingMethod" }, &WithAntiAliasing);

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

}
