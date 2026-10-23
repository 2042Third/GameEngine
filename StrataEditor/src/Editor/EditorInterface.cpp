#include "Editor/EditorInterface.h"

#include "Editor/EditorPreferences.h"

#include <Strata/Core/JsonUtils.h>

#include <nlohmann/json.hpp>

#include <array>
#include <string_view>

namespace Strata::EditorInterface
{

	namespace
	{

		struct ScaleOption
		{
			std::string_view Name;
			float Factor = 1.0f;
		};

		// Percentages of the automatic scale; Automatic is 100%, so the list has no "100%" of its own.
		constexpr std::array<ScaleOption, 8> c_ScaleOptions = { {
			{ "Automatic", 1.0f },
			{ "75%", 0.75f },
			{ "90%", 0.9f },
			{ "110%", 1.1f },
			{ "125%", 1.25f },
			{ "150%", 1.5f },
			{ "175%", 1.75f },
			{ "200%", 2.0f }
		} };

	}

	SettingsSection MakeSettingsSection()
	{
		SettingsSection section;
		section.Id = c_SettingsSection;
		section.Title = "Interface";
		section.Category = "Editor";
		section.Description = "How the editor's interface looks to the person: its scale.";
		section.Scope = SettingsScope::User;

		SettingsField scale;
		scale.Name = c_ScaleField;
		scale.Title = "Scale";
		scale.Description = "The size of the editor's interface as a percentage of the automatic scale, which follows the display's "
			"scaling (Automatic: 100%). It applies at once and on every display; the --ui-scale command-line option wins over it for "
			"its session.";
		scale.Type = SettingsFieldType::Enum;
		for (const ScaleOption& option : c_ScaleOptions)
			scale.Options.emplace_back(option.Name);
		scale.Default = std::string(c_ScaleOptions[0].Name);
		section.Fields.push_back(std::move(scale));
		return section;
	}

	float GetScaleFactor(const EditorPreferences& preferences)
	{
		const nlohmann::json* value = preferences.FindSection(c_SettingsSection);
		const nlohmann::json* scale = value ? JsonUtils::Find(*value, c_ScaleField) : nullptr;
		if (!scale || !scale->is_string())
			return 1.0f;
		const std::string& name = scale->get_ref<const std::string&>();
		for (const ScaleOption& option : c_ScaleOptions)
		{
			if (option.Name == name)
				return option.Factor;
		}
		return 1.0f;
	}

}
