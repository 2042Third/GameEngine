#include <doctest/doctest.h>

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Renderer/QualityTestModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	// The presets as Docs/DeveloperGuide.md, "Quality settings", lays them out, one knob per line in preset order (Low,
	// Medium, High, Ultra): the test's own copy of RendererQuality.cpp's rows, so a knob GetPreset or the section's
	// conversions leave out fails in every column that differs from Medium's.
	constexpr std::array<ShadowMapResolution, 4> c_ShadowResolution = { ShadowMapResolution::Size1024, ShadowMapResolution::Size2048, ShadowMapResolution::Auto, ShadowMapResolution::AutoFine };
	constexpr std::array<uint32_t, 4> c_ShadowCascades = { 3, 4, 4, 4 };
	constexpr std::array<uint32_t, 4> c_LocalShadowAtlasSize = { 2048, 4096, 4096, 8192 };
	constexpr std::array<uint32_t, 4> c_LocalShadowMaxTile = { 512, 1024, 1024, 2048 };
	constexpr std::array<uint32_t, 4> c_MaxShadowedLocalLights = { 4, 16, 16, 16 };
	constexpr std::array<float, 4> c_LODErrorPixels = { 2.0f, 1.0f, 1.0f, 0.5f };
	constexpr std::array<float, 4> c_MinProjectedPixels = { 1.0f, 0.5f, 0.5f, 0.25f };
	constexpr std::array<uint32_t, 4> c_ShadowFilterQuality = { 8, 16, 24, 32 };
	constexpr std::array<uint32_t, 4> c_ContactShadows = { 0, 0, 12, 16 };
	constexpr std::array<AmbientOcclusionLevel, 4> c_AmbientOcclusionQuality = { AmbientOcclusionLevel::Low, AmbientOcclusionLevel::Medium, AmbientOcclusionLevel::High, AmbientOcclusionLevel::Ultra };
	constexpr std::array<uint32_t, 4> c_EnvironmentResolution = { 256, 256, 512, 512 };
	constexpr std::array<ReflectionsQuality, 4> c_Reflections = { ReflectionsQuality::Off, ReflectionsQuality::Checkerboard, ReflectionsQuality::Spatial, ReflectionsQuality::SpatialHigh };
	constexpr std::array<MotionBlurSamples, 4> c_MotionBlurQuality = { MotionBlurSamples::Off, MotionBlurSamples::Samples8, MotionBlurSamples::Samples12,
		MotionBlurSamples::Samples16 };

	void CheckColumn(const RendererQualitySettings& settings, size_t column)
	{
		CAPTURE(column);
		CHECK(settings.ShadowResolution == c_ShadowResolution[column]);
		CHECK(settings.ShadowCascades == c_ShadowCascades[column]);
		CHECK(settings.LocalShadowAtlasSize == c_LocalShadowAtlasSize[column]);
		CHECK(settings.LocalShadowMaxTile == c_LocalShadowMaxTile[column]);
		CHECK(settings.MaxShadowedLocalLights == c_MaxShadowedLocalLights[column]);
		CHECK(settings.LODErrorPixels == c_LODErrorPixels[column]);
		CHECK(settings.MinProjectedPixels == c_MinProjectedPixels[column]);
		CHECK(settings.ShadowFilterQuality == c_ShadowFilterQuality[column]);
		CHECK(settings.ContactShadows == c_ContactShadows[column]);
		CHECK(settings.AmbientOcclusionQuality == c_AmbientOcclusionQuality[column]);
		CHECK(settings.EnvironmentResolution == c_EnvironmentResolution[column]);
		CHECK(settings.Reflections == c_Reflections[column]);
		CHECK(settings.MotionBlurQuality == c_MotionBlurQuality[column]);
		// The players' rows follow no preset.
		CHECK(settings.AntiAliasing == AntiAliasingChoice::Scene);
		CHECK(settings.MotionBlur == SceneEffectChoice::Scene);
		CHECK(settings.DepthOfField == SceneEffectChoice::Scene);
		CHECK(settings.FilmGrain == SceneEffectChoice::Scene);
		CHECK(settings.ChromaticAberration == SceneEffectChoice::Scene);
	}

	constexpr RendererQualityPreset c_Presets[] = { RendererQualityPreset::Low, RendererQualityPreset::Medium, RendererQualityPreset::High,
		RendererQualityPreset::Ultra };

	// The section in a registry of its own, and the values a project would keep.
	struct QualityProject
	{
		SettingsRegistry Registry;
		ProjectSettingsValues Values;

		QualityProject()
		{
			REQUIRE(Registry.Register(RendererQuality::MakeSection()));
		}

		void Set(const nlohmann::json& value)
		{
			std::string error;
			REQUIRE_MESSAGE(Values.Set(RendererQuality::c_SectionId, value, Registry, &error), error);
		}
	};

	// A valid value of the field other than `avoid` (the other bound, option or boolean).
	nlohmann::json OtherValue(const SettingsField& field, const nlohmann::json& avoid)
	{
		switch (field.Type)
		{
			case SettingsFieldType::Bool:
				return !(avoid.is_boolean() && avoid.get<bool>());
			case SettingsFieldType::Int:
				return avoid == nlohmann::json(static_cast<int64_t>(field.Min)) ? static_cast<int64_t>(field.Max) : static_cast<int64_t>(field.Min);
			case SettingsFieldType::Float:
				return avoid == nlohmann::json(field.Min) ? field.Max : field.Min;
			case SettingsFieldType::Enum:
				for (const std::string& option : field.Options)
				{
					if (avoid != nlohmann::json(option))
						return option;
				}
				break;
			case SettingsFieldType::String:
			case SettingsFieldType::Asset:
				break;
		}
		FAIL("Renderer.Quality's field '" << field.Name << "' has no other value");
		return nullptr;
	}

	// The section's validated value.
	nlohmann::json ValidValue(const SettingsSection& section, const nlohmann::json& value)
	{
		std::string error;
		std::optional<nlohmann::json> valid = section.Validate(value, section.Version, &error);
		REQUIRE_MESSAGE(valid.has_value(), error);
		return std::move(*valid);
	}

	// Bytes of a row's memory for a preset's settings at an output size.
	uint64_t MemoryBytes(const RendererQualitySettings& settings, glm::uvec2 output, std::string_view field)
	{
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
		{
			if (row.Field == field)
			{
				REQUIRE(row.MemoryBytes != nullptr);
				return row.MemoryBytes(settings, output);
			}
		}
		FAIL("No row " << std::string(field));
		return 0;
	}

	// The same in whole mebibytes (rounded down).
	uint64_t MemoryMiB(const RendererQualitySettings& settings, glm::uvec2 output, std::string_view field)
	{
		return MemoryBytes(settings, output, field) >> 20;
	}

}

TEST_SUITE("Renderer.Quality")
{
	TEST_CASE("Medium and the defaults are 0.6.0's values, and every preset is its column of the table")
	{
		CheckColumn(RendererQualitySettings {}, 1);
		CheckColumn(RendererQuality::GetPreset(RendererQualityPreset::Medium), 1);
		CheckColumn(SceneRendererSpecification {}.Quality, 1);
		CHECK(RendererQualitySettings {}.Preset == RendererQualityPreset::Medium);

		const SettingsSection section = RendererQuality::MakeSection();
		for (size_t column = 0; column < std::size(c_Presets); column++)
		{
			const RendererQualitySettings preset = RendererQuality::GetPreset(c_Presets[column]);
			CHECK(preset.Preset == c_Presets[column]);
			CheckColumn(preset, column);

			// The same knobs as Custom values survive the section: a knob ToSectionValue or FromSectionValue leaves out comes
			// back as the field's default (High's), which Low's column differs from.
			RendererQualitySettings custom = preset;
			custom.Preset = RendererQualityPreset::Custom;
			std::string error;
			const std::optional<nlohmann::json> value = section.Validate(RendererQuality::ToSectionValue(custom), RendererQuality::c_SectionVersion, &error);
			REQUIRE_MESSAGE(value.has_value(), error);
			const RendererQualitySettings back = RendererQuality::FromSectionValue(&*value);
			CHECK(back.Preset == RendererQualityPreset::Custom);
			CHECK(back.HasSameKnobs(custom));
		}
		// Custom without values is High's knobs.
		const RendererQualitySettings custom = RendererQuality::GetPreset(RendererQualityPreset::Custom);
		CHECK(custom.Preset == RendererQualityPreset::Custom);
		CheckColumn(custom, 2);
	}

	TEST_CASE("Presets keep their numbers and names")
	{
		CHECK(static_cast<int>(RendererQualityPreset::Low) == 0);
		CHECK(static_cast<int>(RendererQualityPreset::Medium) == 1);
		CHECK(static_cast<int>(RendererQualityPreset::High) == 2);
		CHECK(static_cast<int>(RendererQualityPreset::Ultra) == 3);
		CHECK(static_cast<int>(RendererQualityPreset::Custom) == 4);
		for (const RendererQualityPreset preset : { RendererQualityPreset::Low, RendererQualityPreset::Medium, RendererQualityPreset::High,
				 RendererQualityPreset::Ultra, RendererQualityPreset::Custom })
			CHECK(RendererQuality::PresetFromString(RendererQuality::PresetToString(preset)) == preset);
		CHECK(std::string(RendererQuality::PresetToString(RendererQualityPreset::High)) == "High");
		CHECK(RendererQuality::PresetFromString("high", true) == RendererQualityPreset::High);
		CHECK_FALSE(RendererQuality::PresetFromString("high").has_value());
		CHECK_FALSE(RendererQuality::PresetFromString("Extreme", true).has_value());
		CHECK_FALSE(RendererQuality::PresetFromString("").has_value());
		for (const ShadowMapResolution resolution : { ShadowMapResolution::Auto, ShadowMapResolution::AutoFine, ShadowMapResolution::Size1024,
				 ShadowMapResolution::Size2048, ShadowMapResolution::Size4096, ShadowMapResolution::Size8192 })
			CHECK(RendererQuality::ShadowResolutionFromString(RendererQuality::ShadowResolutionToString(resolution)) == resolution);
		CHECK_FALSE(RendererQuality::ShadowResolutionFromString("3000").has_value());
	}

	TEST_CASE("Auto resolves by output rows")
	{
		using RendererQuality::ResolveShadowMapSize;
		CHECK(ResolveShadowMapSize(ShadowMapResolution::Auto, 0) == 2048);
		CHECK(ResolveShadowMapSize(ShadowMapResolution::Auto, 1439) == 2048);
		CHECK(ResolveShadowMapSize(ShadowMapResolution::Auto, 1440) == 4096);
		CHECK(ResolveShadowMapSize(ShadowMapResolution::Auto, 2160) == 4096);
		CHECK(ResolveShadowMapSize(ShadowMapResolution::AutoFine, 1439) == 4096);
		CHECK(ResolveShadowMapSize(ShadowMapResolution::AutoFine, 1440) == 8192);
		for (const uint32_t rows : { 0u, 720u, 1440u, 4320u })
		{
			CHECK(ResolveShadowMapSize(ShadowMapResolution::Size1024, rows) == 1024);
			CHECK(ResolveShadowMapSize(ShadowMapResolution::Size2048, rows) == 2048);
			CHECK(ResolveShadowMapSize(ShadowMapResolution::Size4096, rows) == 4096);
			CHECK(ResolveShadowMapSize(ShadowMapResolution::Size8192, rows) == 8192);
		}
	}

	TEST_CASE("High is Medium below 1440 rows but for the rows that differ at every size")
	{
		// High's sun shadow filter taps, contact shadows, ambient occlusion (3 x 8 samples), prefiltered sky (512 per face),
		// screen-space reflections (Spatial) and motion blur samples differ from Medium's (Strata 0.6.0's costs) at every size; the
		// other knobs equal Medium's below 1440 rows.
		const RendererQualitySettings high = RendererQuality::GetPreset(RendererQualityPreset::High);
		const RendererQualitySettings medium = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		for (const uint32_t rows : { 720u, 1080u, 1439u })
		{
			CAPTURE(rows);
			CHECK(RendererQuality::ResolveShadowMapSize(high.ShadowResolution, rows) == RendererQuality::ResolveShadowMapSize(medium.ShadowResolution, rows));
			RendererQualitySettings resolved = high;
			resolved.ShadowResolution = medium.ShadowResolution;
			// High's wide penumbrae take more taps at every size (the rows streams add that differ below 1440 rows).
			CHECK(resolved.ShadowFilterQuality == 24);
			resolved.ShadowFilterQuality = medium.ShadowFilterQuality;
			CHECK(resolved.ContactShadows == 12);
			CHECK(medium.ContactShadows == 0);
			resolved.ContactShadows = medium.ContactShadows;
			// High's ambient occlusion (3 x 8 samples), prefiltered sky (512 per face) and screen-space reflections (Spatial).
			CHECK(resolved.AmbientOcclusionQuality != medium.AmbientOcclusionQuality);
			resolved.AmbientOcclusionQuality = medium.AmbientOcclusionQuality;
			CHECK(resolved.EnvironmentResolution != medium.EnvironmentResolution);
			resolved.EnvironmentResolution = medium.EnvironmentResolution;
			CHECK(resolved.Reflections != medium.Reflections);
			resolved.Reflections = medium.Reflections;
			// High's motion blur takes 12 samples where Medium's takes 8: a cost only scenes with motion blur pay.
			CHECK(resolved.MotionBlurQuality == MotionBlurSamples::Samples12);
			resolved.MotionBlurQuality = medium.MotionBlurQuality;
			CHECK(resolved.HasSameKnobs(medium));
		}
		CHECK(RendererQuality::ResolveShadowMapSize(high.ShadowResolution, 1440) != RendererQuality::ResolveShadowMapSize(medium.ShadowResolution, 1440));
	}

	TEST_CASE("The section validates its fields")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(RendererQuality::MakeSection()));
		const SettingsSection& section = *registry.Find(RendererQuality::c_SectionId);
		CHECK(section.Title == "Graphics Quality");
		CHECK(section.Category == "Rendering");
		CHECK(section.Version == RendererQuality::c_SectionVersion);
		const nlohmann::json defaults = section.GetDefault();
		CHECK(defaults["Preset"] == "High");
		RendererQualitySettings fromDefaults = RendererQuality::FromSectionValue(&defaults);
		CHECK(fromDefaults.Preset == RendererQualityPreset::High);
		CHECK(fromDefaults.HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::High)));

		const auto refused = [&](const nlohmann::json& changes)
		{
			nlohmann::json value = RendererQuality::ToSectionValue(RendererQuality::GetPreset(RendererQualityPreset::Custom));
			value.update(changes);
			std::string error;
			const bool accepted = section.Validate(value, RendererQuality::c_SectionVersion, &error).has_value();
			CAPTURE(changes.dump());
			CHECK_FALSE(accepted);
			CHECK_FALSE(error.empty());
		};
		refused({ { "Preset", "Extreme" } });
		refused({ { "ShadowCascades", 5 } });
		refused({ { "ShadowCascades", 0 } });
		refused({ { "ShadowResolution", "3000" } });
		refused({ { "LocalShadowMaxTile", "4096" } });
		refused({ { "LODErrorPixels", -1.0 } });
		refused({ { "LODErrorPixels", 9.0 } });
		refused({ { "ShadowFilterQuality", "12" } });
		refused({ { "ShadowFilterQuality", 16 } });
		refused({ { "ContactShadows", 33 } });
		refused({ { "ContactShadows", -1 } });
		refused({ { "ContactShadows", "12" } });
		refused({ { "Unknown", 1 } });
		refused({ { "AmbientOcclusionQuality", "Extreme" } });
		refused({ { "EnvironmentResolution", "1024" } });
		refused({ { "EnvironmentResolution", 512 } });
		refused({ { "Reflections", "Mirror" } });

		const nlohmann::json custom = { { "Preset", "Custom" }, { "ShadowResolution", "4096" }, { "ShadowCascades", 2 }, { "LocalShadowAtlasSize", "2048" },
			{ "LocalShadowMaxTile", "512" }, { "MaxShadowedLocalLights", 8 }, { "LODErrorPixels", 1.5 }, { "MinProjectedPixels", 0.75 },
			{ "AmbientOcclusionQuality", "Low" }, { "EnvironmentResolution", "256" }, { "Reflections", "Off" } };
		std::string error;
		CHECK_MESSAGE(section.Validate(custom, RendererQuality::c_SectionVersion, &error).has_value(), error);
	}

	TEST_CASE("Presets ignore the fields, Custom uses them")
	{
		const nlohmann::json low = { { "Preset", "Low" }, { "ShadowCascades", 1 } };
		const RendererQualitySettings lowSettings = RendererQuality::FromSectionValue(&low);
		CHECK(lowSettings.Preset == RendererQualityPreset::Low);
		CHECK(lowSettings.HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));

		const nlohmann::json custom = { { "Preset", "Custom" }, { "ShadowResolution", "4096" }, { "ShadowCascades", 2 }, { "LocalShadowAtlasSize", "2048" },
			{ "LocalShadowMaxTile", "512" }, { "MaxShadowedLocalLights", 8 }, { "LODErrorPixels", 1.5 }, { "MinProjectedPixels", 0.75 },
			{ "AmbientOcclusionQuality", "Low" }, { "EnvironmentResolution", "256" }, { "Reflections", "Off" } };
		const RendererQualitySettings customSettings = RendererQuality::FromSectionValue(&custom);
		CHECK(customSettings.Preset == RendererQualityPreset::Custom);
		CHECK(customSettings.ShadowResolution == ShadowMapResolution::Size4096);
		CHECK(customSettings.ShadowCascades == 2);
		CHECK(customSettings.LocalShadowAtlasSize == 2048);
		CHECK(customSettings.LocalShadowMaxTile == 512);
		CHECK(customSettings.MaxShadowedLocalLights == 8);
		CHECK(customSettings.LODErrorPixels == 1.5f);
		CHECK(customSettings.MinProjectedPixels == 0.75f);
		CHECK(customSettings.AmbientOcclusionQuality == AmbientOcclusionLevel::Low);
		CHECK(customSettings.EnvironmentResolution == 256);
		CHECK(customSettings.Reflections == ReflectionsQuality::Off);

		const RendererQualitySettings none = RendererQuality::FromSectionValue(nullptr);
		CHECK(none.Preset == RendererQualityPreset::High);
		CHECK(none.HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::High)));
	}

	TEST_CASE("The players' rows are read under every preset")
	{
		for (const char* preset : { "Low", "Medium", "High", "Ultra", "Custom" })
		{
			const nlohmann::json value = { { "Preset", preset }, { "AntiAliasing", "FXAA" }, { "MotionBlur", "Off" }, { "DepthOfField", "Off" },
				{ "FilmGrain", "Off" }, { "ChromaticAberration", "Off" }, { "MotionBlurQuality", "Samples16" } };
			const RendererQualitySettings settings = RendererQuality::FromSectionValue(&value);
			CAPTURE(preset);
			CHECK(settings.AntiAliasing == AntiAliasingChoice::FXAA);
			CHECK(settings.MotionBlur == SceneEffectChoice::Off);
			CHECK(settings.DepthOfField == SceneEffectChoice::Off);
			CHECK(settings.FilmGrain == SceneEffectChoice::Off);
			CHECK(settings.ChromaticAberration == SceneEffectChoice::Off);
			// The samples follow the preset: the value's only under Custom.
			const RendererQualityPreset parsed = *RendererQuality::PresetFromString(preset);
			CHECK(settings.MotionBlurQuality == (parsed == RendererQualityPreset::Custom ? MotionBlurSamples::Samples16
				: RendererQuality::GetPreset(parsed).MotionBlurQuality));
			const nlohmann::json written = RendererQuality::ToSectionValue(settings);
			CHECK(written["AntiAliasing"] == "FXAA");
			CHECK(written["MotionBlur"] == "Off");
			CHECK(written["DepthOfField"] == "Off");
			CHECK(written["FilmGrain"] == "Off");
			CHECK(written["ChromaticAberration"] == "Off");
		}
		CHECK(RendererQuality::GetPreset(RendererQualityPreset::Ultra).DepthOfField == SceneEffectChoice::Scene);
		CHECK(RendererQuality::GetMotionBlurSampleCount(MotionBlurSamples::Off) == 0);
		CHECK(RendererQuality::GetMotionBlurSampleCount(MotionBlurSamples::Samples12) == 12);
		CHECK(RendererQuality::AntiAliasingChoiceFromString("Blurry") == std::nullopt);
		CHECK(RendererQuality::AntiAliasingChoiceFromString("Morphological") == AntiAliasingChoice::Morphological);
		CHECK(std::string(RendererQuality::AntiAliasingChoiceToString(AntiAliasingChoice::Morphological)) == "Morphological");
	}

	TEST_CASE("Screen-space reflections' modes: their names, marches and whether they are spatial")
	{
		// Checkerboard is Strata 0.6.0's trace (24 + 6 steps); the spatial modes march farther per ray on the depth pyramid.
		const ReflectionsQuality modes[] = { ReflectionsQuality::Off, ReflectionsQuality::Checkerboard, ReflectionsQuality::Spatial, ReflectionsQuality::SpatialHigh };
		const char* const names[] = { "Off", "Checkerboard", "Spatial", "SpatialHigh" };
		for (size_t index = 0; index < 4; index++)
		{
			CAPTURE(names[index]);
			CHECK(std::string(RendererQuality::ReflectionsQualityToString(modes[index])) == names[index]);
			CHECK(RendererQuality::ReflectionsQualityFromString(names[index]) == modes[index]);
			CHECK(RendererQuality::IsSpatialReflections(modes[index]) == (index >= 2));
		}
		CHECK_FALSE(RendererQuality::ReflectionsQualityFromString("spatial").has_value());
		const ScreenReflectionSteps checkerboard = RendererQuality::GetScreenReflectionSteps(ReflectionsQuality::Checkerboard);
		CHECK((checkerboard.Steps == 24u && checkerboard.RefineSteps == 6u));
		const ScreenReflectionSteps spatial = RendererQuality::GetScreenReflectionSteps(ReflectionsQuality::Spatial);
		CHECK((spatial.Steps == 32u && spatial.RefineSteps == 6u));
		const ScreenReflectionSteps spatialHigh = RendererQuality::GetScreenReflectionSteps(ReflectionsQuality::SpatialHigh);
		CHECK((spatialHigh.Steps == 48u && spatialHigh.RefineSteps == 8u));
		// An unknown value clamps to Medium's.
		RendererQualitySettings settings;
		settings.Reflections = static_cast<ReflectionsQuality>(7);
		CHECK(RendererQuality::Clamp(settings).Reflections == ReflectionsQuality::Checkerboard);
	}

	TEST_CASE("At 512 no level of the prefiltered sky costs more samples than 256's largest")
	{
		// The atmosphere filters its environment a level per frame (SceneRenderer::c_AtmosphereRefreshSteps): at 512 per face no
		// step may take longer than 0.6.0's largest. Level 0 (roughness 0) copies the cube and takes no samples.
		const auto cost = [](uint32_t size, uint32_t mip)
		{
			const uint64_t texels = std::max(1u, size >> mip);
			return texels * texels * RendererQuality::GetPrefilterSamples(size, mip);
		};
		uint64_t largest = 0;
		for (uint32_t mip = 0; mip < 6; mip++)
		{
			CHECK(RendererQuality::GetPrefilterSamples(256, mip) == 512); // Medium: Strata 0.6.0's
			if (mip > 0)
				largest = std::max(largest, cost(256, mip));
		}
		for (uint32_t mip = 1; mip < 6; mip++)
		{
			CAPTURE(mip);
			CHECK(cost(512, mip) <= largest);
		}
		CHECK(SceneRenderer::c_AtmosphereRefreshSteps == 14);
	}

	TEST_CASE("A value without a field takes the field's default")
	{
		// What later landings rely on: a value saved before a row's field existed is read with the field's default.
		const SettingsSection section = RendererQuality::MakeSection();
		std::string error;
		const std::optional<nlohmann::json> value = section.Validate({ { "Preset", "Custom" } }, 1, &error);
		REQUIRE_MESSAGE(value.has_value(), error);
		const nlohmann::json high = RendererQuality::ToSectionValue(RendererQuality::GetPreset(RendererQualityPreset::Custom));
		CHECK(*value == high);
	}

	TEST_CASE("The project's quality follows IProjectSettings by its version")
	{
		// Without a provider (tests, tools): Medium, SceneRendererSpecification's default.
		CHECK(RendererQuality::GetProjectQuality().Preset == RendererQualityPreset::Medium);
		{
			QualityProject project;
			EngineServices::ScopedProvider<IProjectSettings> provider(project.Values);
			// A project that does not set the section: its default.
			CHECK(RendererQuality::GetProjectQuality().Preset == RendererQualityPreset::High);
			project.Set({ { "Preset", "Low" } });
			CHECK(RendererQuality::GetProjectQuality().Preset == RendererQualityPreset::Low);
			CHECK(RendererQuality::GetProjectQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));
			project.Set({ { "Preset", "Ultra" } });
			CHECK(RendererQuality::GetProjectQuality().Preset == RendererQualityPreset::Ultra);
		}
		CHECK(RendererQuality::GetProjectQuality().Preset == RendererQualityPreset::Medium);
	}

	TEST_CASE("The thumbnails' and the bake's renderers keep their own quality, 0.6.0's")
	{
		// Thumbnails: one 1024-texel cascade and a 2048 atlas, the rest Medium's.
		const RendererQualitySettings thumbnails = SceneRendererSpecification::ForThumbnails().Quality;
		CHECK(thumbnails.Preset == RendererQualityPreset::Custom);
		RendererQualitySettings expected = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		expected.Preset = RendererQualityPreset::Custom;
		expected.ShadowResolution = ShadowMapResolution::Size1024;
		expected.ShadowCascades = 1;
		expected.LocalShadowAtlasSize = 2048;
		CHECK(thumbnails == expected);
		CHECK(RendererQuality::Clamp(thumbnails) == thumbnails);

		// The bake: Medium's cascades and a 2048 atlas (its views draw every shadow view in their first frame).
		const RendererQualitySettings bake = SceneRendererSpecification::ForBake().Quality;
		expected = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		expected.Preset = RendererQualityPreset::Custom;
		expected.LocalShadowAtlasSize = 2048;
		CHECK(bake == expected);
		CHECK(RendererQuality::Clamp(bake) == bake);

		// What else they set: single images without entity IDs, fades or temporal anti-aliasing.
		const SceneRendererSpecification thumbnailRenderer = SceneRendererSpecification::ForThumbnails();
		CHECK(thumbnailRenderer.DebugName == "Thumbnails");
		CHECK_FALSE(thumbnailRenderer.EntityIDs);
		CHECK(thumbnailRenderer.PrefetchDistance == 0.0f);
		CHECK_FALSE(thumbnailRenderer.FadeTextureLevels);
		CHECK_FALSE(thumbnailRenderer.FadeLevelsOfDetail);
		CHECK_FALSE(thumbnailRenderer.TemporalAntiAliasing);
		CHECK_FALSE(thumbnailRenderer.SpreadLightingUpdates);
		CHECK_FALSE(thumbnailRenderer.FitShadowsToView); // A single frame: no readback could fit its cascades
		CHECK(thumbnailRenderer.TextureStreaming);
		// A thumbnail's few objects draw through the CPU path, without the GPU-driven path's fixed costs per frame and scene.
		CHECK(thumbnailRenderer.GPUDriven == GPUDrivenMode::Off);
		CHECK(SceneRendererSpecification::ForBake().DebugName == "ProbeBake");
	}

	TEST_CASE("Clamp keeps every knob in range")
	{
		RendererQualitySettings wild;
		wild.ShadowCascades = 0;
		wild.LocalShadowAtlasSize = 100'000;
		wild.LocalShadowMaxTile = 3'000;
		wild.MaxShadowedLocalLights = 1'000;
		wild.LODErrorPixels = std::numeric_limits<float>::quiet_NaN();
		wild.MinProjectedPixels = -1.0f;
		RendererQualitySettings clamped = RendererQuality::Clamp(wild);
		CHECK(clamped.ShadowCascades == 1);
		CHECK(clamped.LocalShadowAtlasSize == 8192);
		CHECK(clamped.LocalShadowMaxTile == 2048);
		CHECK(clamped.MaxShadowedLocalLights == RenderData::c_MaxShadowedLocalLights);
		CHECK(clamped.LODErrorPixels == 1.0f);
		CHECK(clamped.MinProjectedPixels == 0.5f);

		wild.ShadowCascades = 9;
		wild.LocalShadowAtlasSize = 100;
		wild.LocalShadowMaxTile = 4096;
		wild.LODErrorPixels = 100.0f;
		wild.MinProjectedPixels = std::numeric_limits<float>::infinity();
		clamped = RendererQuality::Clamp(wild);
		CHECK(clamped.ShadowCascades == RenderData::c_MaxCascades);
		CHECK(clamped.LocalShadowAtlasSize == 512);
		CHECK(clamped.LocalShadowMaxTile == 512);
		CHECK(clamped.LODErrorPixels == 8.0f);
		CHECK(clamped.MinProjectedPixels == 0.5f);
		CHECK(RendererQuality::Clamp(RendererQualitySettings {}) == RendererQualitySettings {});

		// The shadow filter's taps: the next option at or above the value, at most 32.
		for (const auto& [taps, expected] : { std::pair { 0u, 8u }, std::pair { 5u, 8u }, std::pair { 8u, 8u }, std::pair { 12u, 16u }, std::pair { 24u, 24u },
				 std::pair { 25u, 32u }, std::pair { 40u, 32u } })
		{
			CAPTURE(taps);
			RendererQualitySettings filter;
			filter.ShadowFilterQuality = taps;
			CHECK(RendererQuality::Clamp(filter).ShadowFilterQuality == expected);
		}
		// Contact shadows: off, or 4 to 32 steps.
		for (const auto& [steps, expected] : { std::pair { 0u, 0u }, std::pair { 1u, 4u }, std::pair { 3u, 4u }, std::pair { 4u, 4u }, std::pair { 12u, 12u },
				 std::pair { 32u, 32u }, std::pair { 33u, 32u }, std::pair { 1000u, 32u } })
		{
			CAPTURE(steps);
			RendererQualitySettings contact;
			contact.ContactShadows = steps;
			CHECK(RendererQuality::Clamp(contact).ContactShadows == expected);
		}
	}

	TEST_CASE("Every field but Preset has a row, and every row a field")
	{
		// A later row's field without its traits line (RendererQuality.cpp's c_RowTraits) fails here.
		const SettingsSection section = RendererQuality::MakeSection();
		const std::span<const RendererQuality::RowTraits> rows = RendererQuality::GetRowTraits();
		REQUIRE(section.Fields.size() == rows.size() + 1);
		CHECK(section.Fields.front().Name == "Preset");
		for (size_t index = 0; index < rows.size(); index++)
		{
			CAPTURE(index);
			CHECK(rows[index].Field == section.Fields[index + 1].Name);
		}
		const RendererQuality::OptionModel model = RendererQuality::GetOptionModel(section);
		CHECK(RendererQuality::FindRow("Preset", model) == nullptr);
		CHECK(RendererQuality::FindRow("NoSuchRow", model) == nullptr);
		const RendererQuality::RowTraits* row = RendererQuality::FindRow("shadowresolution", model);
		REQUIRE(row != nullptr);
		CHECK(row->Field == "ShadowResolution");
		CHECK(row->GPUStage == "GPU.Shadows");
		REQUIRE(model.PresetValues.size() == c_RendererQualityPresetCount);
		for (size_t index = 0; index < c_RendererQualityPresetCount; index++)
			CHECK(model.PresetValues[index] == RendererQuality::ToSectionValue(RendererQuality::GetPreset(c_Presets[index])));
	}

	TEST_CASE("Renderer.Quality's fields are graphics options")
	{
		// What the script host offers players: Bool, Int, Float and Enum fields only.
		for (const SettingsField& field : RendererQuality::MakeSection().Fields)
		{
			CAPTURE(field.Name);
			CHECK((field.Type == SettingsFieldType::Bool || field.Type == SettingsFieldType::Int || field.Type == SettingsFieldType::Float
				|| field.Type == SettingsFieldType::Enum));
		}
	}

	TEST_CASE("The options' texts read in a game's options menu as well as in the editor")
	{
		// Graphics::Options hands every field's title and description to games' menus: they say what an option changes and
		// costs, never where the editor shows it, which engine version it matches, or that it applies under Custom only (a
		// player's change of a row switches to Custom itself).
		for (const SettingsField& field : RendererQuality::MakeSection().Fields)
		{
			CAPTURE(field.Name);
			CHECK_FALSE(field.Title.empty());
			CHECK_FALSE(field.Description.empty());
			for (const char* phrase : { "Custom only", "Strata", "0.6.0", "fields below", "the page", "column" })
			{
				CAPTURE(phrase);
				CHECK(field.Description.find(phrase) == std::string::npos);
			}
		}
	}

	TEST_CASE("Presets round-trip through Custom")
	{
		// Every named preset can be written as Custom fields (what AutoFine exists for), and a switch to Custom keeps its look.
		const SettingsSection section = RendererQuality::MakeSection();
		const RendererQuality::OptionModel model = RendererQuality::GetOptionModel(section);
		for (const RendererQualityPreset preset : c_Presets)
		{
			CAPTURE(RendererQuality::PresetToString(preset));
			const nlohmann::json value = ValidValue(section, { { "Preset", RendererQuality::PresetToString(preset) } });
			const nlohmann::json custom = RendererQuality::MakeCustomStart(value, model);
			CHECK(custom["Preset"] == "Custom");
			CHECK(section.Validate(custom, section.Version).has_value());
			const RendererQualitySettings settings = RendererQuality::FromSectionValue(&custom);
			const RendererQualitySettings expected = RendererQuality::GetPreset(preset);
			CHECK(settings.Preset == RendererQualityPreset::Custom);
			CHECK(settings.HasSameKnobs(expected));
			for (const uint32_t rows : { 720u, 1439u, 1440u, 2160u, 4320u })
			{
				CAPTURE(rows);
				CHECK(RendererQuality::ResolveShadowMapSize(settings.ShadowResolution, rows) == RendererQuality::ResolveShadowMapSize(expected.ShadowResolution, rows));
			}
		}
	}

	TEST_CASE("Normalize spells out a named preset and changes nothing it decides")
	{
		const SettingsSection section = RendererQuality::MakeSection();
		const RendererQuality::OptionModel model = RendererQuality::GetOptionModel(section);
		std::vector<nlohmann::json> values;
		for (const RendererQualityPreset preset : c_Presets)
			values.push_back(ValidValue(section, { { "Preset", RendererQuality::PresetToString(preset) }, { "ShadowCascades", 2 } }));
		values.push_back(ValidValue(section, { { "Preset", "Custom" }, { "ShadowResolution", "4096" }, { "ShadowCascades", 2 }, { "LODErrorPixels", 1.5 } }));
		for (const nlohmann::json& value : values)
		{
			CAPTURE(value.dump());
			const nlohmann::json normalized = RendererQuality::Normalize(value, model);
			CHECK(RendererQuality::FromSectionValue(&normalized).HasSameKnobs(RendererQuality::FromSectionValue(&value)));
			CHECK(RendererQuality::FromSectionValue(&normalized).Preset == RendererQuality::FromSectionValue(&value).Preset);
			CHECK(RendererQuality::Normalize(normalized, model) == normalized);
			CHECK(section.Validate(normalized, section.Version).has_value());
			const std::optional<RendererQualityPreset> preset = RendererQuality::PresetFromString(value["Preset"].get<std::string>());
			REQUIRE(preset.has_value());
			for (const RendererQuality::RowTraits& row : model.Rows)
			{
				CAPTURE(std::string(row.Field));
				const std::string field(row.Field);
				if (*preset == RendererQualityPreset::Custom)
					CHECK(normalized[field] == value[field]);
				else if (row.FollowsPreset)
					CHECK(normalized[field] == model.PresetValues[static_cast<size_t>(*preset)][field]);
			}
		}
		// A value without some fields gets them at their defaults.
		nlohmann::json expected = section.GetDefault();
		expected["Preset"] = "Custom";
		CHECK(RendererQuality::Normalize({ { "Preset", "Custom" } }, model) == expected);
	}

	TEST_CASE("Custom keeps a tuning and otherwise starts from the look in effect")
	{
		const SettingsSection section = RendererQuality::MakeSection();
		const RendererQuality::OptionModel model = RendererQuality::GetOptionModel(section);
		const nlohmann::json& low = model.PresetValues[static_cast<size_t>(RendererQualityPreset::Low)];
		const nlohmann::json& ultra = model.PresetValues[static_cast<size_t>(RendererQualityPreset::Ultra)];
		const auto expectLow = [&](const nlohmann::json& start)
		{
			nlohmann::json expected = low;
			expected["Preset"] = "Custom";
			CHECK(start == expected);
		};

		// The rows at their defaults (nobody tuned them) under Low: Low's look.
		expectLow(RendererQuality::MakeCustomStart(ValidValue(section, { { "Preset", "Low" } }), model));
		// The rows at another preset's values (an earlier start) under Low: Low's look again.
		nlohmann::json ultraRows = ultra;
		ultraRows["Preset"] = "Low";
		expectLow(RendererQuality::MakeCustomStart(ValidValue(section, ultraRows), model));

		// One tuned row: the stored rows are someone's tuning, all kept.
		const nlohmann::json tuned = ValidValue(section, { { "Preset", "Low" }, { "ShadowCascades", 2 } });
		nlohmann::json expected = tuned;
		expected["Preset"] = "Custom";
		CHECK(RendererQuality::MakeCustomStart(tuned, model) == expected);
		// Under Custom nothing but the label changes.
		CHECK(RendererQuality::MakeCustomStart(expected, model) == expected);
	}

	TEST_CASE("Rows that do not follow the preset keep their values in Normalize and Custom's start")
	{
		const Tests::QualityTestModel test;
		const RendererQuality::OptionModel model = test.GetModel();
		const nlohmann::json value = ValidValue(test.Section, { { "Preset", "Low" }, { "Detail", 7 }, { "Comfort", false } });
		CHECK(RendererQuality::Normalize(value, model) == nlohmann::json { { "Preset", "Low" }, { "Detail", 1 }, { "Comfort", false } });
		// Detail 7 is a tuning (neither the default 3 nor a preset's 1 to 4): kept, with the comfort row.
		CHECK(RendererQuality::MakeCustomStart(value, model) == nlohmann::json { { "Preset", "Custom" }, { "Detail", 7 }, { "Comfort", false } });
		const nlohmann::json untuned = ValidValue(test.Section, { { "Preset", "Ultra" }, { "Comfort", false } });
		CHECK(RendererQuality::MakeCustomStart(untuned, model) == nlohmann::json { { "Preset", "Custom" }, { "Detail", 4 }, { "Comfort", false } });
	}

	TEST_CASE("Rows that do not follow the preset are read under every preset")
	{
		// A players' comfort row (FollowsPreset false) is read from the value under every preset (its line in
		// FromSectionValue's last block); a row that follows the preset is the preset's whatever the value holds.
		const SettingsSection section = RendererQuality::MakeSection();
		const RendererQuality::OptionModel model = RendererQuality::GetOptionModel(section);
		for (size_t presetIndex = 0; presetIndex < c_RendererQualityPresetCount; presetIndex++)
		{
			const nlohmann::json& presetValue = model.PresetValues[presetIndex];
			for (size_t fieldIndex = 1; fieldIndex < section.Fields.size(); fieldIndex++)
			{
				const SettingsField& field = section.Fields[fieldIndex];
				const RendererQuality::RowTraits* row = RendererQuality::FindRow(field.Name, model);
				REQUIRE(row != nullptr);
				CAPTURE(presetIndex);
				CAPTURE(field.Name);
				// A value other than the default and other than the preset's.
				nlohmann::json other = OtherValue(field, field.Default);
				if (other == presetValue[field.Name])
					other = OtherValue(field, other);
				const nlohmann::json value = ValidValue(section, { { "Preset", RendererQuality::PresetToString(c_Presets[presetIndex]) }, { field.Name, other } });
				const nlohmann::json read = RendererQuality::ToSectionValue(RendererQuality::FromSectionValue(&value));
				if (row->FollowsPreset)
					CHECK(read[field.Name] == presetValue[field.Name]);
				else
					CHECK(read[field.Name] == other);
			}
		}
	}

	TEST_CASE("Memory estimates")
	{
		// Cascades: cascades x size^2 x 4 bytes (D32), Auto resolved by the output's rows; the local atlas: size^2 x 4.
		const RendererQualitySettings medium = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		const RendererQualitySettings high = RendererQuality::GetPreset(RendererQualityPreset::High);
		const RendererQualitySettings ultra = RendererQuality::GetPreset(RendererQualityPreset::Ultra);
		CHECK(MemoryMiB(high, { 3840, 2160 }, "ShadowResolution") == 256);
		CHECK(MemoryMiB(high, { 3840, 2160 }, "LocalShadowAtlasSize") == 64);
		CHECK(MemoryMiB(high, { 1920, 1080 }, "ShadowResolution") == 64);
		CHECK(MemoryMiB(ultra, { 3840, 2160 }, "ShadowResolution") == 1024);
		CHECK(MemoryMiB(ultra, { 3840, 2160 }, "LocalShadowAtlasSize") == 256);
		CHECK(MemoryMiB(medium, { 1920, 1080 }, "ShadowResolution") == 64);
		CHECK(MemoryMiB(medium, { 1920, 1080 }, "LocalShadowAtlasSize") == 64);
		// The motion blur's targets, for a scene that blurs: RGBA16F and R32_UINT per pixel (12 bytes) and two RGBA16F tile
		// targets of ceil(size / 8) texels; none without samples or with the players' Motion blur off.
		const RendererQuality::RowTraits* motionBlur = nullptr;
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
			motionBlur = row.Field == "MotionBlurQuality" ? &row : motionBlur;
		REQUIRE(motionBlur);
		REQUIRE(motionBlur->MemoryBytes);
		CHECK(motionBlur->MemoryBytes(medium, { 1920, 1080 }) == 1920ull * 1080 * 12 + 2ull * 240 * 135 * 8);
		CHECK(motionBlur->MemoryBytes(high, { 3840, 2160 }) == 3840ull * 2160 * 12 + 2ull * 480 * 270 * 8);
		CHECK(motionBlur->MemoryBytes(high, { 1921, 1081 }) == 1921ull * 1081 * 12 + 2ull * 241 * 136 * 8);
		CHECK(motionBlur->MemoryBytes(RendererQuality::GetPreset(RendererQualityPreset::Low), { 3840, 2160 }) == 0);
		RendererQualitySettings playerOff = high;
		playerOff.MotionBlur = SceneEffectChoice::Off;
		CHECK(motionBlur->MemoryBytes(playerOff, { 3840, 2160 }) == 0);
		// The sky's prefiltered cube: six RGBA16F faces with six levels, 4 MiB at 256 per face and 16 MiB at 512, whatever the
		// output's size (Clamp's 256 for any other value).
		const auto texelsOfLevels = [](uint64_t size)
		{
			uint64_t texels = 0;
			for (uint32_t level = 0; level < 6; level++)
				texels += std::max<uint64_t>(1, size >> level) * std::max<uint64_t>(1, size >> level);
			return texels;
		};
		CHECK(MemoryBytes(medium, { 3840, 2160 }, "EnvironmentResolution") == texelsOfLevels(256) * 6 * 8);
		CHECK(MemoryBytes(high, { 1920, 1080 }, "EnvironmentResolution") == texelsOfLevels(512) * 6 * 8);
		CHECK(texelsOfLevels(256) * 6 * 8 == 4193280); // Just under 4 MiB: the last level is 8 texels each way
		CHECK(MemoryBytes(ultra, { 3840, 2160 }, "EnvironmentResolution") == 16773120);
		RendererQualitySettings oddSky = high;
		oddSky.EnvironmentResolution = 300;
		CHECK(MemoryBytes(oddSky, { 3840, 2160 }, "EnvironmentResolution") == texelsOfLevels(256) * 6 * 8);
		// Screen-space reflections, for a scene that asks for them: Checkerboard's RGBA16F surface target and half-resolution
		// reflections; the spatial modes' RGBA16F surface target, traced and resolved reflections, their R11G11B10F full-resolution
		// color copy, and the half-resolution copy's six levels (1920 x 1080 down to 60 x 33 at 3840 x 2160); none when off.
		CHECK(MemoryBytes(medium, { 3840, 2160 }, "Reflections") == 3840ull * 2160 * 8 + 1920ull * 1080 * 8);
		CHECK(MemoryMiB(medium, { 3840, 2160 }, "Reflections") == 79);
		CHECK(MemoryBytes(medium, { 1921, 1081 }, "Reflections") == 1921ull * 1081 * 8 + 961ull * 541 * 8);
		const uint64_t halfCopy4K = 1920ull * 1080 + 960 * 540 + 480 * 270 + 240 * 135 + 120 * 67 + 60 * 33;
		CHECK(MemoryBytes(high, { 3840, 2160 }, "Reflections") == 3840ull * 2160 * 28 + halfCopy4K * 4);
		CHECK(MemoryMiB(high, { 3840, 2160 }, "Reflections") == 232);
		const uint64_t halfCopy1080 = 960ull * 540 + 480 * 270 + 240 * 135 + 120 * 67 + 60 * 33 + 30 * 16;
		CHECK(MemoryBytes(ultra, { 1920, 1080 }, "Reflections") == 1920ull * 1080 * 28 + halfCopy1080 * 4);
		CHECK(MemoryBytes(RendererQuality::GetPreset(RendererQualityPreset::Low), { 3840, 2160 }, "Reflections") == 0);
		// The rows that allocate nothing of their own have no estimate.
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
		{
			CAPTURE(std::string(row.Field));
			CHECK((row.MemoryBytes != nullptr) == (row.Field == "ShadowResolution" || row.Field == "LocalShadowAtlasSize" || row.Field == "EnvironmentResolution"
				|| row.Field == "Reflections" || row.Field == "MotionBlurQuality"));
		}
	}
}
