#pragma once

#include <Strata/Core/SettingsRegistry.h>

#include <cstdint>

namespace Strata
{

	class EditorPreferences;

	// How fast the editor redraws: the Editor.FrameRate preference section, a simple section drawn by the generic
	// Preferences page. The editor counts as idle once nothing happened for IdleAfterSeconds (0: never) and then redraws at
	// the idle rates; while anything happens it redraws at most at ActiveFrameRate (0: unlimited). EditorLayer reads the
	// policy whenever the preferences change; headless, --frames and command-script runs ignore it.
	namespace EditorFrameRate
	{

		inline constexpr const char* c_SettingsSection = "Editor.FrameRate";
		inline constexpr const char* c_IdleAfterSecondsField = "IdleAfterSeconds";
		inline constexpr const char* c_IdleFrameRateField = "IdleFrameRate";
		inline constexpr const char* c_UnfocusedIdleFrameRateField = "UnfocusedIdleFrameRate";
		inline constexpr const char* c_ActiveFrameRateField = "ActiveFrameRate";

		// 0.7.0's constants, now the defaults.
		inline constexpr double c_DefaultIdleAfterSeconds = 0.5;
		inline constexpr uint32_t c_DefaultIdleFrameRate = 30;
		inline constexpr uint32_t c_DefaultUnfocusedIdleFrameRate = 10;
		inline constexpr double c_MaxIdleAfterSeconds = 3600.0;
		// No rate below this is offered: FramePacer sleeps without waking on input, so a lower rate would hold the first
		// input back for more than a fifth of a second.
		inline constexpr uint32_t c_MinFrameRate = 5;

		struct Policy
		{
			double IdleAfterSeconds = c_DefaultIdleAfterSeconds; // 0: never idle
			uint32_t IdleFrameRate = c_DefaultIdleFrameRate;
			uint32_t UnfocusedIdleFrameRate = c_DefaultUnfocusedIdleFrameRate;
			uint32_t ActiveFrameRate = 0; // 0: unlimited

			bool operator==(const Policy& other) const = default;
		};

		// The section (a user section, registered by EditorPreferences::RegisterSections).
		SettingsSection MakeSettingsSection();
		// The policy the preferences choose; the defaults while the section is not set or its stored value is refused.
		Policy GetPolicy(const EditorPreferences& preferences);

	}

}
