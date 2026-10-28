#include <doctest/doctest.h>

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/SceneRenderer.h"

#include "Renderer/QualityTestModel.h"

#include <nlohmann/json.hpp>

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

	// Mebibytes of the rows' memory for a preset's settings at an output size.
	uint64_t MemoryMiB(const RendererQualitySettings& settings, glm::uvec2 output, std::string_view field)
	{
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
		{
			if (row.Field == field)
			{
				REQUIRE(row.MemoryBytes != nullptr);
				return row.MemoryBytes(settings, output) >> 20;
			}
		}
		FAIL("No row " << std::string(field));
		return 0;
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

	TEST_CASE("High is Medium below 1440 rows")
	{
		const RendererQualitySettings high = RendererQuality::GetPreset(RendererQualityPreset::High);
		const RendererQualitySettings medium = RendererQuality::GetPreset(RendererQualityPreset::Medium);
		for (const uint32_t rows : { 720u, 1080u, 1439u })
		{
			CAPTURE(rows);
			CHECK(RendererQuality::ResolveShadowMapSize(high.ShadowResolution, rows) == RendererQuality::ResolveShadowMapSize(medium.ShadowResolution, rows));
			RendererQualitySettings resolved = high;
			resolved.ShadowResolution = medium.ShadowResolution;
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
		// The rows that allocate nothing of their own have no estimate.
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
		{
			CAPTURE(std::string(row.Field));
			CHECK((row.MemoryBytes != nullptr) == (row.Field == "ShadowResolution" || row.Field == "LocalShadowAtlasSize"));
		}
	}
}
