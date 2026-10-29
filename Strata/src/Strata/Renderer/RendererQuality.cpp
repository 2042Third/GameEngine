#include "stpch.h"
#include "Strata/Renderer/RendererQuality.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/PostProcessData.h"

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
		constexpr PresetRow<uint32_t> c_ShadowFilterQualityRow = { 8, 16, 24, 32 };
		constexpr PresetRow<uint32_t> c_ContactShadowsRow = { 0, 0, 12, 16 };
		constexpr PresetRow<MotionBlurSamples> c_MotionBlurQualityRow = { MotionBlurSamples::Off, MotionBlurSamples::Samples8, MotionBlurSamples::Samples12, MotionBlurSamples::Samples16 };

		// GPU memory of the rows that allocate (RowTraits::MemoryBytes), as the renderer makes the resources.
		uint64_t CascadeBytes(const RendererQualitySettings& settings, glm::uvec2 output)
		{
			// A D32 layer per cascade, of the size the output's rows resolve Auto and AutoFine to.
			const uint64_t size = RendererQuality::ResolveShadowMapSize(settings.ShadowResolution, output.y);
			return static_cast<uint64_t>(settings.ShadowCascades) * size * size * 4;
		}

		uint64_t LocalShadowAtlasBytes(const RendererQualitySettings& settings, glm::uvec2)
		{
			// The point and spot lights' D32 atlas (SceneRendererLocalShadows.cpp).
			const uint64_t size = settings.LocalShadowAtlasSize;
			return size * size * 4;
		}

		uint64_t MotionBlurBytes(const RendererQualitySettings& settings, glm::uvec2 output)
		{
			// For a scene that blurs (SceneRenderer::EnsureMotionBlurResources): the RGBA16F output and the R32_UINT pixel target,
			// viewport-sized, and the two RGBA16F tile targets. With no samples, or the players' Motion blur Off, none.
			if (settings.MotionBlurQuality == MotionBlurSamples::Off || settings.MotionBlur == SceneEffectChoice::Off)
				return 0;
			const uint64_t pixels = static_cast<uint64_t>(output.x) * output.y;
			const uint64_t tiles = static_cast<uint64_t>((output.x + RenderData::c_MotionBlurMinTileSize - 1) / RenderData::c_MotionBlurMinTileSize)
				* ((output.y + RenderData::c_MotionBlurMinTileSize - 1) / RenderData::c_MotionBlurMinTileSize);
			return pixels * (8 + 4) + 2 * tiles * 8;
		}

		// What graphics options know of each field but Preset, in field order: one line per row, beside its PresetRow line.
		constexpr RendererQuality::RowTraits c_RowTraits[] = {
			{ "ShadowResolution", true, "GPU.Shadows", &CascadeBytes },
			{ "ShadowCascades", true, "GPU.Shadows", nullptr }, // Counted with ShadowResolution
			{ "LocalShadowAtlasSize", true, "GPU.Shadows", &LocalShadowAtlasBytes },
			{ "LocalShadowMaxTile", true, "GPU.Shadows", nullptr },
			{ "MaxShadowedLocalLights", true, "GPU.Shadows", nullptr },
			{ "LODErrorPixels", true, "GPU.Opaque", nullptr },
			{ "MinProjectedPixels", true, "GPU.Opaque", nullptr },
			{ "ShadowFilterQuality", true, "GPU.ScreenShadows", nullptr }, // The filter runs in the mask's pass; taps allocate nothing
			{ "ContactShadows", true, "GPU.ScreenShadows", nullptr },      // Marches through the mask's own two targets
			{ "MotionBlurQuality", true, "GPU.MotionBlur", &MotionBlurBytes },
			// The players' rows: comfort and preference, not cost.
			{ "AntiAliasing", false, "", nullptr },                 // Its cost is the chosen method's (GPU.TAA or GPU.Post)
			{ "MotionBlur", false, "GPU.MotionBlur", nullptr },     // Counted with MotionBlurQuality
			{ "DepthOfField", false, "GPU.DepthOfField", nullptr }, // The scene's lens sizes its targets, not the quality
			{ "FilmGrain", false, "GPU.Post", nullptr },            // Added by the pass that writes the display image
			{ "ChromaticAberration", false, "GPU.Post", nullptr },  // Applied by the tone mapping
		};

		constexpr uint32_t c_MinLocalShadowAtlasSize = 512;
		constexpr uint32_t c_MaxLocalShadowAtlasSize = 8192;
		constexpr uint32_t c_MinLocalShadowTile = 128;
		constexpr float c_MaxPixels = 8.0f;
		// Contact shadows march no steps (off) or 4 to 32 (RendererQualitySettings::ContactShadows): fewer reach too little.
		constexpr uint32_t c_MinContactShadowSteps = 4;
		constexpr uint32_t c_MaxContactShadowSteps = 32;

		// The sizes the section offers for the local shadows' atlas and its largest tile, stored as their names.
		constexpr uint32_t c_AtlasSizeOptions[] = { 1024, 2048, 4096, 8192 };
		constexpr uint32_t c_MaxTileOptions[] = { 256, 512, 1024, 2048 };
		// The taps the sun's wide penumbrae may take (RendererQualitySettings::ShadowFilterQuality), stored as their names.
		constexpr uint32_t c_ShadowFilterTapOptions[] = { 8, 16, 24, 32 };

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
		constexpr ResolutionName c_ResolutionNames[] = { { ShadowMapResolution::Auto, "Auto" }, { ShadowMapResolution::AutoFine, "AutoFine" },
			{ ShadowMapResolution::Size1024, "1024" }, { ShadowMapResolution::Size2048, "2048" }, { ShadowMapResolution::Size4096, "4096" },
			{ ShadowMapResolution::Size8192, "8192" } };

		// The names of an enum row's values, in value order (the files and scripts store the names).
		constexpr const char* c_MotionBlurSamplesNames[] = { "Off", "Samples8", "Samples12", "Samples16" };
		constexpr uint32_t c_MotionBlurSampleCounts[] = { 0, 8, 12, 16 };
		constexpr const char* c_AntiAliasingChoiceNames[] = { "Scene", "Off", "FXAA", "TAA", "Morphological" };
		constexpr const char* c_SceneEffectChoiceNames[] = { "Scene", "Off" };

		template<typename T, size_t N>
		const char* EnumToString(T value, const char* const (&names)[N])
		{
			const size_t index = static_cast<size_t>(value);
			return index < N ? names[index] : names[0];
		}

		template<typename T, size_t N>
		std::optional<T> EnumFromString(std::string_view name, const char* const (&names)[N])
		{
			for (size_t index = 0; index < N; index++)
			{
				if (name == names[index])
					return static_cast<T>(index);
			}
			return std::nullopt;
		}

		template<size_t N>
		std::vector<std::string> EnumOptions(const char* const (&names)[N])
		{
			return std::vector<std::string>(std::begin(names), std::end(names));
		}

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

		// An enum row stored as its value's name.
		template<typename T, size_t N>
		void ReadEnumField(const nlohmann::json& value, std::string_view name, const char* const (&names)[N], T& knob)
		{
			const nlohmann::json* member = JsonUtils::Find(value, name);
			if (member && member->is_string())
			{
				if (std::optional<T> parsed = EnumFromString<T>(member->get_ref<const std::string&>(), names))
					knob = *parsed;
			}
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

		// The value with a member for every field of the model's section (absent ones at their defaults).
		nlohmann::json WithEveryField(const nlohmann::json& value, const RendererQuality::OptionModel& model)
		{
			nlohmann::json result = value.is_object() ? value : nlohmann::json::object();
			if (!model.Section)
				return result;
			for (const SettingsField& field : model.Section->Fields)
			{
				if (!result.contains(field.Name))
					result[field.Name] = field.Default;
			}
			return result;
		}

		// The named preset a value is under (nullopt under Custom, or without a readable Preset), and its values in the model
		// (null when the model has none for it).
		const nlohmann::json* FindNamedPresetValue(const nlohmann::json& value, const RendererQuality::OptionModel& model)
		{
			const nlohmann::json* member = JsonUtils::Find(value, "Preset");
			if (!member || !member->is_string())
				return nullptr;
			const std::optional<RendererQualityPreset> preset = RendererQuality::PresetFromString(member->get_ref<const std::string&>());
			if (!preset || *preset == RendererQualityPreset::Custom)
				return nullptr;
			const size_t index = static_cast<size_t>(*preset);
			return index < model.PresetValues.size() ? &model.PresetValues[index] : nullptr;
		}

		// Whether every row that follows the preset has the same value in `a` and `b` (a member missing from either differs).
		bool HaveSameFollowingRows(const nlohmann::json& a, const nlohmann::json& b, const RendererQuality::OptionModel& model)
		{
			for (const RendererQuality::RowTraits& row : model.Rows)
			{
				if (!row.FollowsPreset)
					continue;
				const nlohmann::json* left = JsonUtils::Find(a, row.Field);
				const nlohmann::json* right = JsonUtils::Find(b, row.Field);
				if (!left || !right || *left != *right)
					return false;
			}
			return true;
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
					settings.ShadowFilterQuality = c_ShadowFilterQualityRow[index];
					settings.ContactShadows = c_ContactShadowsRow[index];
					settings.MotionBlurQuality = c_MotionBlurQualityRow[index];
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
			// The next option at or above the value, at most the largest.
			const auto taps = std::find_if(std::begin(c_ShadowFilterTapOptions), std::end(c_ShadowFilterTapOptions),
				[&](uint32_t option) { return option >= clamped.ShadowFilterQuality; });
			clamped.ShadowFilterQuality = taps != std::end(c_ShadowFilterTapOptions) ? *taps : c_ShadowFilterTapOptions[std::size(c_ShadowFilterTapOptions) - 1];
			clamped.ContactShadows = clamped.ContactShadows == 0 ? 0u
				: std::clamp(clamped.ContactShadows, c_MinContactShadowSteps, c_MaxContactShadowSteps);
			if (static_cast<size_t>(clamped.MotionBlurQuality) >= std::size(c_MotionBlurSamplesNames))
				clamped.MotionBlurQuality = medium.MotionBlurQuality;
			if (static_cast<size_t>(clamped.AntiAliasing) >= std::size(c_AntiAliasingChoiceNames))
				clamped.AntiAliasing = AntiAliasingChoice::Scene;
			if (static_cast<size_t>(clamped.MotionBlur) >= std::size(c_SceneEffectChoiceNames))
				clamped.MotionBlur = SceneEffectChoice::Scene;
			if (static_cast<size_t>(clamped.DepthOfField) >= std::size(c_SceneEffectChoiceNames))
				clamped.DepthOfField = SceneEffectChoice::Scene;
			if (static_cast<size_t>(clamped.FilmGrain) >= std::size(c_SceneEffectChoiceNames))
				clamped.FilmGrain = SceneEffectChoice::Scene;
			if (static_cast<size_t>(clamped.ChromaticAberration) >= std::size(c_SceneEffectChoiceNames))
				clamped.ChromaticAberration = SceneEffectChoice::Scene;
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

		const char* MotionBlurSamplesToString(MotionBlurSamples samples)
		{
			return EnumToString(samples, c_MotionBlurSamplesNames);
		}

		std::optional<MotionBlurSamples> MotionBlurSamplesFromString(std::string_view name)
		{
			return EnumFromString<MotionBlurSamples>(name, c_MotionBlurSamplesNames);
		}

		uint32_t GetMotionBlurSampleCount(MotionBlurSamples samples)
		{
			const size_t index = static_cast<size_t>(samples);
			return index < std::size(c_MotionBlurSampleCounts) ? c_MotionBlurSampleCounts[index] : 0;
		}

		const char* AntiAliasingChoiceToString(AntiAliasingChoice choice)
		{
			return EnumToString(choice, c_AntiAliasingChoiceNames);
		}

		std::optional<AntiAliasingChoice> AntiAliasingChoiceFromString(std::string_view name)
		{
			return EnumFromString<AntiAliasingChoice>(name, c_AntiAliasingChoiceNames);
		}

		const char* SceneEffectChoiceToString(SceneEffectChoice choice)
		{
			return EnumToString(choice, c_SceneEffectChoiceNames);
		}

		std::optional<SceneEffectChoice> SceneEffectChoiceFromString(std::string_view name)
		{
			return EnumFromString<SceneEffectChoice>(name, c_SceneEffectChoiceNames);
		}

		SettingsSection MakeSection()
		{
			const RendererQualitySettings high = GetPreset(RendererQualityPreset::High);
			SettingsSection section;
			section.Id = std::string(c_SectionId);
			section.Title = "Graphics Quality";
			section.Category = "Rendering";
			section.Description = "The rendering quality of the project's games and of the editor's views and captures: the costly knobs every "
				"renderer draws with, as a preset or as Custom values. Medium keeps Strata 0.6.0's costs (its sample counts and resolutions); "
				"High, the default, adds more sun shadow filter taps and contact shadows at every size and draws 4096-texel sun shadow "
				"cascades from 1440 output rows. Scenes keep their look on lights, Post Process and the sky; "
				"no setting renders below the output's resolution. Set it with project.setSettings; players may change it in games that offer "
				"graphics options, and StrataRuntime --quality for one run.";
			section.Scope = SettingsScope::Project;
			section.Version = c_SectionVersion;

			std::vector<std::string> presets;
			for (const PresetName& entry : c_PresetNames)
				presets.emplace_back(entry.Name);
			// The fields' titles and descriptions reach players too (a game's options menu lists them, Graphics::Options): they say
			// what an option changes and what it costs, never engine versions or where the editor shows them.
			section.Fields.push_back(EnumField("Preset", "Preset", "The quality preset: Low, Medium, High or Ultra sets every other option to its "
				"value; Custom uses the values chosen for them.", std::move(presets), "High"));
			std::vector<std::string> resolutions;
			for (const ResolutionName& entry : c_ResolutionNames)
				resolutions.emplace_back(entry.Name);
			// One field per knob, in the rows' order, each defaulting to High's value: choosing Custom from High changes nothing.
			section.Fields.push_back(EnumField("ShadowResolution", "Shadow resolution", "Texels each way of every sun shadow cascade: sharper sun "
				"shadows for more GPU memory and time. Auto: 2048 below 1440 output rows, 4096 from 1440 (about one texel per screen pixel at each "
				"cascade's far end); AutoFine: 4096 and 8192. Memory per view: cascades x size x size x 4 bytes (4 x 4096: 256 MiB).",
				std::move(resolutions), ShadowResolutionToString(high.ShadowResolution)));
			section.Fields.push_back(NumberField("ShadowCascades", "Shadow cascades", "Sun shadow cascades: fewer cost less GPU time and memory but "
				"spread the same texels over the shadow distance.", SettingsFieldType::Int, 1, RenderData::c_MaxCascades, high.ShadowCascades));
			section.Fields.push_back(EnumField("LocalShadowAtlasSize", "Point and spot shadow atlas", "Texels each way of the shadow atlas point and "
				"spot lights share: sharper shadows of those lights for more GPU memory (4096: 64 MiB per view).", SizeOptions(c_AtlasSizeOptions),
				std::to_string(high.LocalShadowAtlasSize)));
			section.Fields.push_back(EnumField("LocalShadowMaxTile", "Largest point and spot shadow tile", "The largest tile of the atlas a point or "
				"spot light's shadow gets (at most the atlas): tiles follow how large the light looks on screen, and larger ones are sharper.",
				SizeOptions(c_MaxTileOptions), std::to_string(high.LocalShadowMaxTile)));
			section.Fields.push_back(NumberField("MaxShadowedLocalLights", "Shadowed point and spot lights", "Point and spot lights that cast "
				"shadows in a view, the most relevant first: more cost more GPU time.", SettingsFieldType::Int, 0, RenderData::c_MaxShadowedLocalLights,
				high.MaxShadowedLocalLights));
			section.Fields.push_back(NumberField("LODErrorPixels", "Level of detail error (pixels)", "Levels of detail are chosen so that their "
				"error spans at most this many pixels on screen: lower draws more triangles (0: full detail).", SettingsFieldType::Float, 0.0,
				c_MaxPixels, high.LODErrorPixels));
			section.Fields.push_back(NumberField("MinProjectedPixels", "Smallest drawn size (pixels)", "Meshes smaller on screen than this many "
				"pixels are not drawn: higher draws fewer (0: all).", SettingsFieldType::Float, 0.0, c_MaxPixels, high.MinProjectedPixels));
			section.Fields.push_back(EnumField("ShadowFilterQuality", "Shadow filter quality", "Taps of the sun's shadow edges where they are wider "
				"than a shadow texel: more taps leave less grain in soft shadows for more GPU time (8, 16, 24 or 32).",
				SizeOptions(c_ShadowFilterTapOptions), std::to_string(high.ShadowFilterQuality)));
			section.Fields.push_back(NumberField("ContactShadows", "Contact shadows", "Steps of the sun's contact shadows, which ground small objects "
				"below the shadow map's detail: more steps find thinner contacts for more GPU time (0: off; 1 to 3 count as 4).",
				SettingsFieldType::Int, 0, c_MaxContactShadowSteps, high.ContactShadows));
			section.Fields.push_back(EnumField("MotionBlurQuality", "Motion blur samples", "Samples per pixel of the motion blur, in scenes that "
				"use it: more samples show less streak noise where things move fast, for more GPU time (Off: no motion blur).",
				EnumOptions(c_MotionBlurSamplesNames), MotionBlurSamplesToString(high.MotionBlurQuality)));
			// The players' rows: read under every preset.
			section.Fields.push_back(EnumField("AntiAliasing", "Anti-aliasing", "How jagged edges are smoothed: Scene uses the game's own method; "
				"Off is the sharpest and costs nothing; FXAA and Morphological smooth each frame on its own (Morphological keeps more texture "
				"detail); TAA also calms shimmer by blending earlier frames. Each costs a little GPU time.", EnumOptions(c_AntiAliasingChoiceNames),
				AntiAliasingChoiceToString(high.AntiAliasing)));
			section.Fields.push_back(EnumField("MotionBlur", "Motion blur", "Scene shows the game's motion blur where it uses it; Off never blurs "
				"what moves and saves its GPU time.", EnumOptions(c_SceneEffectChoiceNames), SceneEffectChoiceToString(high.MotionBlur)));
			section.Fields.push_back(EnumField("DepthOfField", "Depth of field", "Scene shows the depth of field of the game's cameras that use it; "
				"Off keeps every distance in focus and saves its GPU time.", EnumOptions(c_SceneEffectChoiceNames),
				SceneEffectChoiceToString(high.DepthOfField)));
			section.Fields.push_back(EnumField("FilmGrain", "Film grain", "Scene shows the game's film grain where it uses it; Off never adds "
				"grain.", EnumOptions(c_SceneEffectChoiceNames), SceneEffectChoiceToString(high.FilmGrain)));
			section.Fields.push_back(EnumField("ChromaticAberration", "Chromatic aberration", "Scene shows the color fringes toward the image's "
				"corners where the game uses them; Off never.", EnumOptions(c_SceneEffectChoiceNames), SceneEffectChoiceToString(high.ChromaticAberration)));
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
				ReadField(*value, "ShadowFilterQuality", settings.ShadowFilterQuality);
				ReadField(*value, "ContactShadows", settings.ContactShadows);
				ReadEnumField(*value, "MotionBlurQuality", c_MotionBlurSamplesNames, settings.MotionBlurQuality);
			}
			// Knobs read under every preset (players' comfort rows) follow here, one line each.
			if (value)
			{
				ReadEnumField(*value, "AntiAliasing", c_AntiAliasingChoiceNames, settings.AntiAliasing);
				ReadEnumField(*value, "MotionBlur", c_SceneEffectChoiceNames, settings.MotionBlur);
				ReadEnumField(*value, "DepthOfField", c_SceneEffectChoiceNames, settings.DepthOfField);
				ReadEnumField(*value, "FilmGrain", c_SceneEffectChoiceNames, settings.FilmGrain);
				ReadEnumField(*value, "ChromaticAberration", c_SceneEffectChoiceNames, settings.ChromaticAberration);
			}
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
				{ "ShadowFilterQuality", std::to_string(settings.ShadowFilterQuality) },
				{ "ContactShadows", settings.ContactShadows },
				{ "MotionBlurQuality", MotionBlurSamplesToString(settings.MotionBlurQuality) },
				{ "AntiAliasing", AntiAliasingChoiceToString(settings.AntiAliasing) },
				{ "MotionBlur", SceneEffectChoiceToString(settings.MotionBlur) },
				{ "DepthOfField", SceneEffectChoiceToString(settings.DepthOfField) },
				{ "FilmGrain", SceneEffectChoiceToString(settings.FilmGrain) },
				{ "ChromaticAberration", SceneEffectChoiceToString(settings.ChromaticAberration) }
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

		std::span<const RowTraits> GetRowTraits()
		{
			return c_RowTraits;
		}

		std::span<const nlohmann::json> GetPresetValues()
		{
			static const std::array<nlohmann::json, c_RendererQualityPresetCount> s_Values = []()
			{
				std::array<nlohmann::json, c_RendererQualityPresetCount> values;
				for (size_t index = 0; index < c_RendererQualityPresetCount; index++)
					values[index] = ToSectionValue(GetPreset(static_cast<RendererQualityPreset>(index)));
				return values;
			}();
			return s_Values;
		}

		OptionModel GetOptionModel(const SettingsSection& section)
		{
			OptionModel model;
			model.Section = &section;
			model.Rows = GetRowTraits();
			model.PresetValues = GetPresetValues();
			return model;
		}

		const RowTraits* FindRow(std::string_view field, const OptionModel& model)
		{
			for (const RowTraits& row : model.Rows)
			{
				if (EqualsIgnoringCase(row.Field, field))
					return &row;
			}
			return nullptr;
		}

		nlohmann::json Normalize(const nlohmann::json& value, const OptionModel& model)
		{
			nlohmann::json result = WithEveryField(value, model);
			const nlohmann::json* presetValue = FindNamedPresetValue(result, model);
			if (!presetValue)
				return result;
			// Under a named preset the rows that follow it are the preset's, whatever is stored for them.
			for (const RowTraits& row : model.Rows)
			{
				if (!row.FollowsPreset)
					continue;
				if (const nlohmann::json* member = JsonUtils::Find(*presetValue, row.Field))
					result[std::string(row.Field)] = *member;
			}
			return result;
		}

		nlohmann::json MakeCustomStart(const nlohmann::json& value, const OptionModel& model)
		{
			const nlohmann::json stored = WithEveryField(value, model);
			// Rows that hold the defaults or a preset's values were not tuned (nobody chose them, or a preset's look was the
			// start): Custom starts from the look in effect. Anything else is someone's tuning, kept.
			bool tuned = model.Section && !HaveSameFollowingRows(stored, model.Section->GetDefault(), model);
			for (const nlohmann::json& presetValue : model.PresetValues)
				tuned = tuned && !HaveSameFollowingRows(stored, presetValue, model);
			nlohmann::json result = tuned ? stored : Normalize(stored, model);
			result["Preset"] = PresetToString(RendererQualityPreset::Custom);
			return result;
		}

	}

}
