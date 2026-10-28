#include "stpch.h"
#include "Strata/Renderer/RendererQuality.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace Strata
{

	namespace
	{

		// The presets, a knob per line with its four values in preset order, so that rows added by different changes are
		// separate lines. Medium is Strata 0.6.0's costs.
		template<typename T>
		using PresetRow = std::array<T, c_RendererQualityPresetCount>; // Low, Medium (0.6.0), High, Ultra

		constexpr PresetRow<ShadowMapResolution> c_ShadowResolutionRow = { ShadowMapResolution::Size1024, ShadowMapResolution::Size2048, ShadowMapResolution::Auto, ShadowMapResolution::AutoFine };
		constexpr PresetRow<uint32_t> c_ShadowCascadesRow = { 3, 4, 4, 4 };
		constexpr PresetRow<uint32_t> c_LocalShadowAtlasSizeRow = { 2048, 4096, 4096, 8192 };
		constexpr PresetRow<uint32_t> c_LocalShadowMaxTileRow = { 512, 1024, 1024, 2048 };
		constexpr PresetRow<uint32_t> c_MaxShadowedLocalLightsRow = { 4, 16, 16, 16 };
		constexpr PresetRow<float> c_LODErrorPixelsRow = { 2.0f, 1.0f, 1.0f, 0.5f };
		constexpr PresetRow<float> c_MinProjectedPixelsRow = { 1.0f, 0.5f, 0.5f, 0.25f };
		constexpr PresetRow<AmbientOcclusionLevel> c_AmbientOcclusionQualityRow = { AmbientOcclusionLevel::Low, AmbientOcclusionLevel::Medium, AmbientOcclusionLevel::High, AmbientOcclusionLevel::Ultra };

		constexpr uint32_t c_MinLocalShadowAtlasSize = 512;
		constexpr uint32_t c_MaxLocalShadowAtlasSize = 8192;
		constexpr uint32_t c_MinLocalShadowTile = 128;
		constexpr float c_MaxPixels = 8.0f;

		// The sizes the section offers for the local shadows' atlas and its largest tile, stored as their names.
		constexpr uint32_t c_AtlasSizeOptions[] = { 1024, 2048, 4096, 8192 };
		constexpr uint32_t c_MaxTileOptions[] = { 256, 512, 1024, 2048 };

		struct PresetName
		{
			RendererQualityPreset Preset;
			const char* Name;
		};
		constexpr PresetName c_PresetNames[] = { { RendererQualityPreset::Low, "Low" }, { RendererQualityPreset::Medium, "Medium" },
			{ RendererQualityPreset::High, "High" }, { RendererQualityPreset::Ultra, "Ultra" }, { RendererQualityPreset::Custom, "Custom" } };

		struct ResolutionName
		{
			ShadowMapResolution Resolution;
			const char* Name;
		};
		struct AmbientOcclusionName
		{
			AmbientOcclusionLevel Level;
			const char* Name;
			AmbientOcclusionSamples Samples;
		};
		constexpr AmbientOcclusionName c_AmbientOcclusionNames[] = { { AmbientOcclusionLevel::Low, "Low", { 1, 4 } },
			{ AmbientOcclusionLevel::Medium, "Medium", { 2, 6 } }, { AmbientOcclusionLevel::High, "High", { 3, 8 } },
			{ AmbientOcclusionLevel::Ultra, "Ultra", { 4, 12 } } };

		constexpr ResolutionName c_ResolutionNames[] = { { ShadowMapResolution::Auto, "Auto" }, { ShadowMapResolution::AutoFine, "AutoFine" },
			{ ShadowMapResolution::Size1024, "1024" }, { ShadowMapResolution::Size2048, "2048" }, { ShadowMapResolution::Size4096, "4096" },
			{ ShadowMapResolution::Size8192, "8192" } };

		uint32_t FloorPowerOfTwo(uint32_t value)
		{
			uint32_t power = 1;
			while (power <= value / 2)
				power *= 2;
			return power;
		}

		float ClampPixels(float value, float fallback)
		{
			return std::isfinite(value) && value >= 0.0f ? std::min(value, c_MaxPixels) : fallback;
		}

		bool EqualsIgnoringCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
			{
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		// A member of a validated value into a knob; a missing or unreadable member keeps the knob's value.
		void ReadField(const nlohmann::json& value, std::string_view name, ShadowMapResolution& knob)
		{
			const nlohmann::json* member = JsonUtils::Find(value, name);
			if (member && member->is_string())
			{
				if (std::optional<ShadowMapResolution> resolution = RendererQuality::ShadowResolutionFromString(member->get_ref<const std::string&>()))
					knob = *resolution;
			}
		}

		void ReadField(const nlohmann::json& value, std::string_view name, AmbientOcclusionLevel& knob)
		{
			const nlohmann::json* member = JsonUtils::Find(value, name);
			if (member && member->is_string())
			{
				if (std::optional<AmbientOcclusionLevel> level = RendererQuality::AmbientOcclusionLevelFromString(member->get_ref<const std::string&>()))
					knob = *level;
			}
		}

		void ReadField(const nlohmann::json& value, std::string_view name, uint32_t& knob)
		{
			const nlohmann::json* member = JsonUtils::Find(value, name);
			if (!member)
				return;
			// The atlas and tile sizes are stored as the names of their options ("4096"), the counts as numbers.
			if (member->is_string())
			{
				const std::string& text = member->get_ref<const std::string&>();
				uint32_t parsed = 0;
				const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
				if (error == std::errc() && end == text.data() + text.size())
					knob = parsed;
			}
			else if (member->is_number_integer() && member->get<int64_t>() >= 0)
			{
				knob = static_cast<uint32_t>(std::min<int64_t>(member->get<int64_t>(), UINT32_MAX));
			}
		}

		void ReadField(const nlohmann::json& value, std::string_view name, float& knob)
		{
			const nlohmann::json* member = JsonUtils::Find(value, name);
			if (member && member->is_number())
				knob = member->get<float>();
		}

		SettingsField EnumField(const char* name, const char* title, std::string description, std::vector<std::string> options, std::string defaultValue)
		{
			SettingsField field;
			field.Name = name;
			field.Title = title;
			field.Description = std::move(description);
			field.Type = SettingsFieldType::Enum;
			field.Options = std::move(options);
			field.Default = std::move(defaultValue);
			return field;
		}

		SettingsField NumberField(const char* name, const char* title, std::string description, SettingsFieldType type, double min, double max, double defaultValue)
		{
			SettingsField field;
			field.Name = name;
			field.Title = title;
			field.Description = std::move(description);
			field.Type = type;
			field.Min = min;
			field.Max = max;
			field.Default = type == SettingsFieldType::Int ? nlohmann::json(static_cast<int64_t>(defaultValue)) : nlohmann::json(defaultValue);
			return field;
		}

		std::vector<std::string> SizeOptions(std::span<const uint32_t> sizes)
		{
			std::vector<std::string> options;
			for (const uint32_t size : sizes)
				options.push_back(std::to_string(size));
			return options;
		}

	}

	bool RendererQualitySettings::HasSameKnobs(const RendererQualitySettings& other) const
	{
		RendererQualitySettings a = *this;
		RendererQualitySettings b = other;
		a.Preset = RendererQualityPreset::Custom;
		b.Preset = RendererQualityPreset::Custom;
		return a == b;
	}

	namespace RendererQuality
	{

		RendererQualitySettings GetPreset(RendererQualityPreset preset)
		{
			static const std::array<RendererQualitySettings, c_RendererQualityPresetCount> s_Presets = []()
			{
				std::array<RendererQualitySettings, c_RendererQualityPresetCount> presets = {};
				for (size_t index = 0; index < c_RendererQualityPresetCount; index++)
				{
					RendererQualitySettings& settings = presets[index];
					settings.Preset = static_cast<RendererQualityPreset>(index);
					settings.ShadowResolution = c_ShadowResolutionRow[index];
					settings.ShadowCascades = c_ShadowCascadesRow[index];
					settings.LocalShadowAtlasSize = c_LocalShadowAtlasSizeRow[index];
					settings.LocalShadowMaxTile = c_LocalShadowMaxTileRow[index];
					settings.MaxShadowedLocalLights = c_MaxShadowedLocalLightsRow[index];
					settings.LODErrorPixels = c_LODErrorPixelsRow[index];
					settings.MinProjectedPixels = c_MinProjectedPixelsRow[index];
					settings.AmbientOcclusionQuality = c_AmbientOcclusionQualityRow[index];
				}
				return presets;
			}();
			if (preset == RendererQualityPreset::Custom)
			{
				RendererQualitySettings custom = s_Presets[static_cast<size_t>(RendererQualityPreset::High)];
				custom.Preset = RendererQualityPreset::Custom;
				return custom;
			}
			const size_t index = static_cast<size_t>(preset);
			return index < s_Presets.size() ? s_Presets[index] : s_Presets[static_cast<size_t>(RendererQualityPreset::Medium)];
		}

		RendererQualitySettings Clamp(const RendererQualitySettings& settings)
		{
			const RendererQualitySettings medium = GetPreset(RendererQualityPreset::Medium);
			RendererQualitySettings clamped = settings;
			if (static_cast<uint8_t>(clamped.ShadowResolution) > static_cast<uint8_t>(ShadowMapResolution::Size8192))
				clamped.ShadowResolution = medium.ShadowResolution;
			clamped.ShadowCascades = std::clamp<uint32_t>(clamped.ShadowCascades, 1, RenderData::c_MaxCascades);
			clamped.LocalShadowAtlasSize = FloorPowerOfTwo(std::clamp(clamped.LocalShadowAtlasSize, c_MinLocalShadowAtlasSize, c_MaxLocalShadowAtlasSize));
			clamped.LocalShadowMaxTile = FloorPowerOfTwo(std::clamp(clamped.LocalShadowMaxTile, c_MinLocalShadowTile, clamped.LocalShadowAtlasSize));
			clamped.MaxShadowedLocalLights = std::min(clamped.MaxShadowedLocalLights, RenderData::c_MaxShadowedLocalLights);
			clamped.LODErrorPixels = ClampPixels(clamped.LODErrorPixels, medium.LODErrorPixels);
			clamped.MinProjectedPixels = ClampPixels(clamped.MinProjectedPixels, medium.MinProjectedPixels);
			if (static_cast<uint8_t>(clamped.AmbientOcclusionQuality) > static_cast<uint8_t>(AmbientOcclusionLevel::Ultra))
				clamped.AmbientOcclusionQuality = medium.AmbientOcclusionQuality;
			if (static_cast<uint8_t>(clamped.Preset) > static_cast<uint8_t>(RendererQualityPreset::Custom))
				clamped.Preset = RendererQualityPreset::Custom;
			return clamped;
		}

		uint32_t ResolveShadowMapSize(ShadowMapResolution resolution, uint32_t outputRows)
		{
			const bool high = outputRows >= c_HighResolutionRows;
			switch (resolution)
			{
				case ShadowMapResolution::Auto: return high ? 4096 : 2048;
				case ShadowMapResolution::AutoFine: return high ? 8192 : 4096;
				case ShadowMapResolution::Size1024: return 1024;
				case ShadowMapResolution::Size2048: return 2048;
				case ShadowMapResolution::Size4096: return 4096;
				case ShadowMapResolution::Size8192: return 8192;
			}
			return 2048;
		}

		const char* PresetToString(RendererQualityPreset preset)
		{
			for (const PresetName& entry : c_PresetNames)
			{
				if (entry.Preset == preset)
					return entry.Name;
			}
			return "Custom";
		}

		std::optional<RendererQualityPreset> PresetFromString(std::string_view name, bool ignoreCase)
		{
			for (const PresetName& entry : c_PresetNames)
			{
				if (ignoreCase ? EqualsIgnoringCase(name, entry.Name) : name == entry.Name)
					return entry.Preset;
			}
			return std::nullopt;
		}

		const char* ShadowResolutionToString(ShadowMapResolution resolution)
		{
			for (const ResolutionName& entry : c_ResolutionNames)
			{
				if (entry.Resolution == resolution)
					return entry.Name;
			}
			return "2048";
		}

		std::optional<ShadowMapResolution> ShadowResolutionFromString(std::string_view name)
		{
			for (const ResolutionName& entry : c_ResolutionNames)
			{
				if (name == entry.Name)
					return entry.Resolution;
			}
			return std::nullopt;
		}

		const char* AmbientOcclusionLevelToString(AmbientOcclusionLevel level)
		{
			for (const AmbientOcclusionName& entry : c_AmbientOcclusionNames)
			{
				if (entry.Level == level)
					return entry.Name;
			}
			return "Medium";
		}

		std::optional<AmbientOcclusionLevel> AmbientOcclusionLevelFromString(std::string_view name)
		{
			for (const AmbientOcclusionName& entry : c_AmbientOcclusionNames)
			{
				if (name == entry.Name)
					return entry.Level;
			}
			return std::nullopt;
		}

		AmbientOcclusionSamples GetAmbientOcclusionSamples(AmbientOcclusionLevel level)
		{
			for (const AmbientOcclusionName& entry : c_AmbientOcclusionNames)
			{
				if (entry.Level == level)
					return entry.Samples;
			}
			return AmbientOcclusionSamples {};
		}


		SettingsSection MakeSection()
		{
			const RendererQualitySettings high = GetPreset(RendererQualityPreset::High);
			SettingsSection section;
			section.Id = std::string(c_SectionId);
			section.Title = "Graphics Quality";
			section.Category = "Rendering";
			section.Description = "The rendering quality of the project's games and of the editor's views and captures: the costly knobs every "
				"renderer draws with, as a preset or as Custom values. Medium keeps Strata 0.6.0's costs; High, the default, draws 4096-texel "
				"sun shadow cascades from 1440 output rows (as Medium below them). Scenes keep their look on lights, Post Process and the sky; "
				"no setting renders below the output's resolution. Set it with project.setSettings; players may change it in games that offer "
				"graphics options, and StrataRuntime --quality for one run.";
			section.Scope = SettingsScope::Project;
			section.Version = c_SectionVersion;

			std::vector<std::string> presets;
			for (const PresetName& entry : c_PresetNames)
				presets.emplace_back(entry.Name);
			section.Fields.push_back(EnumField("Preset", "Preset", "The preset: Low, Medium (Strata 0.6.0's costs), High (the default), Ultra, or "
				"Custom, which uses the fields below. Under a preset the fields are ignored.", std::move(presets), "High"));
			std::vector<std::string> resolutions;
			for (const ResolutionName& entry : c_ResolutionNames)
				resolutions.emplace_back(entry.Name);
			// One field per knob, in the rows' order, each defaulting to High's value: choosing Custom from High changes nothing.
			section.Fields.push_back(EnumField("ShadowResolution", "Shadow resolution", "Custom only: texels each way of every sun shadow cascade. "
				"Auto: 2048 below 1440 output rows, 4096 from 1440 (about one texel per screen pixel at each cascade's far end); AutoFine: 4096 and "
				"8192. Memory per view: cascades x size x size x 4 bytes (4 x 4096: 256 MiB).", std::move(resolutions),
				ShadowResolutionToString(high.ShadowResolution)));
			section.Fields.push_back(NumberField("ShadowCascades", "Shadow cascades", "Custom only: sun shadow cascades. Fewer cost less but spread the "
				"same texels over the shadow distance.", SettingsFieldType::Int, 1, RenderData::c_MaxCascades, high.ShadowCascades));
			section.Fields.push_back(EnumField("LocalShadowAtlasSize", "Point and spot shadow atlas", "Custom only: texels each way of the shadow "
				"atlas point and spot lights share (4096: 64 MiB per view).", SizeOptions(c_AtlasSizeOptions), std::to_string(high.LocalShadowAtlasSize)));
			section.Fields.push_back(EnumField("LocalShadowMaxTile", "Largest point and spot shadow tile", "Custom only: the largest tile a point or "
				"spot light's shadow view gets (at most the atlas); tiles follow how large the light looks on screen.", SizeOptions(c_MaxTileOptions),
				std::to_string(high.LocalShadowMaxTile)));
			section.Fields.push_back(NumberField("MaxShadowedLocalLights", "Shadowed point and spot lights", "Custom only: point and spot lights "
				"that cast shadows in a view, the most relevant first.", SettingsFieldType::Int, 0, RenderData::c_MaxShadowedLocalLights,
				high.MaxShadowedLocalLights));
			section.Fields.push_back(NumberField("LODErrorPixels", "Level of detail error (pixels)", "Custom only: levels of detail are chosen so "
				"that their error spans at most this many pixels on screen (0: full detail).", SettingsFieldType::Float, 0.0, c_MaxPixels,
				high.LODErrorPixels));
			section.Fields.push_back(NumberField("MinProjectedPixels", "Smallest drawn size (pixels)", "Custom only: meshes smaller on screen than "
				"this many pixels are not drawn (0: all).", SettingsFieldType::Float, 0.0, c_MaxPixels, high.MinProjectedPixels));
			std::vector<std::string> occlusionLevels;
			for (const AmbientOcclusionName& entry : c_AmbientOcclusionNames)
				occlusionLevels.emplace_back(entry.Name);
			section.Fields.push_back(EnumField("AmbientOcclusionQuality", "Ambient occlusion quality", "Custom only: ambient occlusion's samples, "
				"Low 1x4, Medium 2x6 (Strata 0.6.0's), High 3x8, Ultra 4x12 slices x steps per side; GPU time at 3840x2160 grows about with their "
				"product.", std::move(occlusionLevels), AmbientOcclusionLevelToString(high.AmbientOcclusionQuality)));
			return section;
		}

		RendererQualitySettings FromSectionValue(const nlohmann::json* value)
		{
			// The preset: the value's, else the section's default (High). Members are read with JsonUtils::Find; a missing or
			// unreadable member keeps the struct's value (callers pass validated values, which have every field).
			RendererQualityPreset preset = RendererQualityPreset::High;
			if (value)
			{
				const nlohmann::json* member = JsonUtils::Find(*value, "Preset");
				if (member && member->is_string())
					preset = PresetFromString(member->get_ref<const std::string&>()).value_or(RendererQualityPreset::High);
			}
			RendererQualitySettings settings = GetPreset(preset);
			if (value && preset == RendererQualityPreset::Custom)
			{
				// One line per knob that follows the preset, in the rows' order.
				ReadField(*value, "ShadowResolution", settings.ShadowResolution);
				ReadField(*value, "ShadowCascades", settings.ShadowCascades);
				ReadField(*value, "LocalShadowAtlasSize", settings.LocalShadowAtlasSize);
				ReadField(*value, "LocalShadowMaxTile", settings.LocalShadowMaxTile);
				ReadField(*value, "MaxShadowedLocalLights", settings.MaxShadowedLocalLights);
				ReadField(*value, "LODErrorPixels", settings.LODErrorPixels);
				ReadField(*value, "MinProjectedPixels", settings.MinProjectedPixels);
				ReadField(*value, "AmbientOcclusionQuality", settings.AmbientOcclusionQuality);
			}
			// Knobs read under every preset (players' comfort rows) follow here, one line each.
			return Clamp(settings);
		}

		nlohmann::json ToSectionValue(const RendererQualitySettings& settings)
		{
			return nlohmann::json {
				{ "Preset", PresetToString(settings.Preset) },
				{ "ShadowResolution", ShadowResolutionToString(settings.ShadowResolution) },
				{ "ShadowCascades", settings.ShadowCascades },
				{ "LocalShadowAtlasSize", std::to_string(settings.LocalShadowAtlasSize) },
				{ "LocalShadowMaxTile", std::to_string(settings.LocalShadowMaxTile) },
				{ "MaxShadowedLocalLights", settings.MaxShadowedLocalLights },
				{ "LODErrorPixels", settings.LODErrorPixels },
				{ "MinProjectedPixels", settings.MinProjectedPixels },
				{ "AmbientOcclusionQuality", AmbientOcclusionLevelToString(settings.AmbientOcclusionQuality) }
			};
		}

		const RendererQualitySettings& GetProjectQuality()
		{
			static const RendererQualitySettings s_WithoutProvider = GetPreset(RendererQualityPreset::Medium);
			struct Cache
			{
				bool Valid = false;
				uint64_t SettingsVersion = 0;
				RendererQualitySettings Quality;
			};
			static Cache s_Cache;
			const IProjectSettings* settings = EngineServices::Get<IProjectSettings>();
			if (!settings)
				return s_WithoutProvider;
			const uint64_t version = settings->GetVersion();
			if (s_Cache.Valid && s_Cache.SettingsVersion == version)
				return s_Cache.Quality;
			// The provider holds validated values only (a value its section refuses is not provided: the default applies).
			s_Cache.Quality = FromSectionValue(settings->FindSection(c_SectionId));
			s_Cache.Valid = true;
			s_Cache.SettingsVersion = version;
			return s_Cache.Quality;
		}

	}

}
