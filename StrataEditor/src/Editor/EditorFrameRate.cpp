#include "Editor/EditorFrameRate.h"

#include "Editor/EditorPreferences.h"

#include <Strata/Core/JsonUtils.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <span>
#include <string>
#include <string_view>

namespace Strata::EditorFrameRate
{

	namespace
	{

		struct RateOption
		{
			std::string_view Name;
			uint32_t FramesPerSecond = 0; // 0: unlimited
		};

		constexpr RateOption c_IdleRates[] = { { "60", 60 }, { "30", 30 }, { "20", 20 }, { "15", 15 }, { "10", 10 }, { "5", 5 } };
		constexpr RateOption c_UnfocusedIdleRates[] = { { "60", 60 }, { "30", 30 }, { "15", 15 }, { "10", 10 }, { "5", 5 } };
		constexpr RateOption c_ActiveRates[] = { { "Unlimited", 0 }, { "240", 240 }, { "165", 165 }, { "144", 144 }, { "120", 120 }, { "90", 90 },
			{ "60", 60 } };

		SettingsField MakeRateField(const char* name, std::string title, std::string description, std::span<const RateOption> options,
			std::string_view defaultName)
		{
			SettingsField field;
			field.Name = name;
			field.Title = std::move(title);
			field.Description = std::move(description);
			field.Type = SettingsFieldType::Enum;
			for (const RateOption& option : options)
				field.Options.emplace_back(option.Name);
			field.Default = std::string(defaultName);
			return field;
		}

		// The rate a stored option names; `fallback` for anything else (validation refuses other names before they are stored).
		uint32_t ReadRate(const nlohmann::json& section, const char* name, std::span<const RateOption> options, uint32_t fallback)
		{
			const nlohmann::json* value = JsonUtils::Find(section, name);
			if (!value || !value->is_string())
				return fallback;
			const std::string& text = value->get_ref<const std::string&>();
			for (const RateOption& option : options)
			{
				if (option.Name == text)
					return option.FramesPerSecond;
			}
			return fallback;
		}

	}

	SettingsSection MakeSettingsSection()
	{
		SettingsSection section;
		section.Id = c_SettingsSection;
		section.Title = "Frame Rate";
		section.Category = "Editor";
		section.Description = "How fast the editor redraws: as fast as allowed while anything happens, slower while idle. Applies at once to "
			"editors with a window; headless, --frames and command-script runs ignore it.";
		section.Scope = SettingsScope::User;
		section.Version = 1;

		SettingsField idleAfter;
		idleAfter.Name = c_IdleAfterSecondsField;
		idleAfter.Title = "Idle after (seconds)";
		idleAfter.Description = "How long after the last input (and after the last automation request, at least 1 s) the editor counts as idle "
			"and redraws at the idle frame rate. 0 never counts as idle: the editor redraws at the active frame rate all the time, also behind "
			"other windows, which with an unlimited active rate is hundreds of frames per second on a fast machine. Not the --idle-timeout "
			"option, which closes a headless editor without clients.";
		idleAfter.Type = SettingsFieldType::Float;
		idleAfter.Min = 0.0;
		idleAfter.Max = c_MaxIdleAfterSeconds;
		idleAfter.DragSpeed = 0.1;
		idleAfter.Default = c_DefaultIdleAfterSeconds;
		section.Fields.push_back(std::move(idleAfter));

		section.Fields.push_back(MakeRateField(c_IdleFrameRateField, "Idle frame rate",
			"Frames per second while idle with the editor's window focused. No effect while Idle after is 0.", c_IdleRates, "30"));
		section.Fields.push_back(MakeRateField(c_UnfocusedIdleFrameRateField, "Idle frame rate in the background",
			"Frames per second while idle and another window has the focus. No effect while Idle after is 0.", c_UnfocusedIdleRates, "10"));
		section.Fields.push_back(MakeRateField(c_ActiveFrameRateField, "Active frame rate",
			"The most frames per second while anything happens (input, a running game, loading, an animation). Unlimited leaves the pace to "
			"the display's vertical sync where the graphics device paces frames, otherwise as fast as frames go. play.step's steps and "
			"commands that wait for frames always run uncapped. A cap above the display's refresh rate shows nothing more.",
			c_ActiveRates, "Unlimited"));
		return section;
	}

	Policy GetPolicy(const EditorPreferences& preferences)
	{
		Policy policy;
		const nlohmann::json* section = preferences.FindSection(c_SettingsSection);
		if (!section || !section->is_object())
			return policy;
		if (const nlohmann::json* idleAfter = JsonUtils::Find(*section, c_IdleAfterSecondsField); idleAfter && idleAfter->is_number())
		{
			const double seconds = idleAfter->get<double>();
			if (std::isfinite(seconds) && seconds >= 0.0 && seconds <= c_MaxIdleAfterSeconds)
				policy.IdleAfterSeconds = seconds;
		}
		policy.IdleFrameRate = ReadRate(*section, c_IdleFrameRateField, c_IdleRates, c_DefaultIdleFrameRate);
		policy.UnfocusedIdleFrameRate = ReadRate(*section, c_UnfocusedIdleFrameRateField, c_UnfocusedIdleRates, c_DefaultUnfocusedIdleFrameRate);
		policy.ActiveFrameRate = ReadRate(*section, c_ActiveFrameRateField, c_ActiveRates, 0);
		return policy;
	}

}
