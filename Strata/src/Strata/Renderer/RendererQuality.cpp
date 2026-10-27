#include "stpch.h"
#include "Strata/Renderer/RendererQuality.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

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

		constexpr uint32_t c_MinLocalShadowAtlasSize = 512;
		constexpr uint32_t c_MaxLocalShadowAtlasSize = 8192;
		constexpr uint32_t c_MinLocalShadowTile = 128;
		constexpr float c_MaxPixels = 8.0f;

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

	}

}
