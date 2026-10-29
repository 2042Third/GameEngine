#pragma once

#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Renderer/RendererQuality.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>

namespace Strata::Tests
{

	// A simple quality section of a test's own, with the option model graphics options work over (RendererQuality::
	// OptionModel): Preset (Low ... Custom, default High), "Detail" (Int 0 to 8, follows the preset: Low 1, Medium 2,
	// High 3, Ultra 4) and "Comfort" (Bool, default true, a players' choice the presets leave alone). So the page's and the
	// players' options' tests cover a row that does not follow the preset before the engine's section has one. Not movable:
	// the model points into it.
	struct QualityTestModel
	{
		static constexpr const char* c_SectionId = "Test.Quality";

		SettingsSection Section;
		std::array<RendererQuality::RowTraits, 2> Rows = { {
			{ "Detail", true, "GPU.Opaque", nullptr },
			{ "Comfort", false, {}, nullptr },
		} };
		std::array<nlohmann::json, c_RendererQualityPresetCount> PresetValues;

		explicit QualityTestModel(uint32_t version = 1)
		{
			Section.Id = c_SectionId;
			Section.Title = "Test Quality";
			Section.Category = "Rendering";
			Section.Description = "A quality section of the tests.";
			Section.Version = version;

			SettingsField preset;
			preset.Name = "Preset";
			preset.Title = "Preset";
			preset.Description = "The preset.";
			preset.Type = SettingsFieldType::Enum;
			preset.Options = { "Low", "Medium", "High", "Ultra", "Custom" };
			preset.Default = "High";
			Section.Fields.push_back(preset);

			SettingsField detail;
			detail.Name = "Detail";
			detail.Title = "Detail";
			detail.Description = "Custom only: how much detail.";
			detail.Type = SettingsFieldType::Int;
			detail.Min = 0;
			detail.Max = 8;
			detail.Default = 3;
			Section.Fields.push_back(detail);

			SettingsField comfort;
			comfort.Name = "Comfort";
			comfort.Title = "Comfort";
			comfort.Description = "A players' choice under every preset.";
			comfort.Type = SettingsFieldType::Bool;
			comfort.Default = true;
			Section.Fields.push_back(comfort);

			const char* names[] = { "Low", "Medium", "High", "Ultra" };
			for (size_t index = 0; index < PresetValues.size(); index++)
				PresetValues[index] = { { "Preset", names[index] }, { "Detail", static_cast<int64_t>(index) + 1 }, { "Comfort", true } };
		}

		QualityTestModel(const QualityTestModel&) = delete;
		QualityTestModel& operator=(const QualityTestModel&) = delete;

		RendererQuality::OptionModel GetModel() const
		{
			RendererQuality::OptionModel model;
			model.Section = &Section;
			model.Rows = Rows;
			model.PresetValues = PresetValues;
			return model;
		}
	};

}
