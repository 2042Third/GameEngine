#include <doctest/doctest.h>

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/SceneRenderer.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

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

	TEST_CASE("High is Medium below 1440 rows but for the sun shadows' filter and contact shadows")
	{
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

		const nlohmann::json custom = { { "Preset", "Custom" }, { "ShadowResolution", "4096" }, { "ShadowCascades", 2 }, { "LocalShadowAtlasSize", "2048" },
			{ "LocalShadowMaxTile", "512" }, { "MaxShadowedLocalLights", 8 }, { "LODErrorPixels", 1.5 }, { "MinProjectedPixels", 0.75 } };
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
			{ "LocalShadowMaxTile", "512" }, { "MaxShadowedLocalLights", 8 }, { "LODErrorPixels", 1.5 }, { "MinProjectedPixels", 0.75 } };
		const RendererQualitySettings customSettings = RendererQuality::FromSectionValue(&custom);
		CHECK(customSettings.Preset == RendererQualityPreset::Custom);
		CHECK(customSettings.ShadowResolution == ShadowMapResolution::Size4096);
		CHECK(customSettings.ShadowCascades == 2);
		CHECK(customSettings.LocalShadowAtlasSize == 2048);
		CHECK(customSettings.LocalShadowMaxTile == 512);
		CHECK(customSettings.MaxShadowedLocalLights == 8);
		CHECK(customSettings.LODErrorPixels == 1.5f);
		CHECK(customSettings.MinProjectedPixels == 0.75f);

		const RendererQualitySettings none = RendererQuality::FromSectionValue(nullptr);
		CHECK(none.Preset == RendererQualityPreset::High);
		CHECK(none.HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::High)));
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
		CHECK(thumbnailRenderer.TextureStreaming);
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
}
