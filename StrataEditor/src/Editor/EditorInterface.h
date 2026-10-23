#pragma once

#include <Strata/Core/SettingsRegistry.h>

namespace Strata
{

	class EditorPreferences;

	// How the person wants the editor's interface: the Editor.Interface preference section, a simple section drawn by the
	// generic Preferences page. Its Scale is a percentage of the automatic UI scale (the display's), which EditorLayer gives
	// the host (EditorHost::SetUIScaleFactor) whenever the preferences change; --ui-scale wins for its session.
	namespace EditorInterface
	{

		inline constexpr const char* c_SettingsSection = "Editor.Interface";
		inline constexpr const char* c_ScaleField = "Scale";

		// The section (a user section, registered by EditorPreferences::RegisterSections).
		SettingsSection MakeSettingsSection();

		// The factor of the automatic UI scale the preferences choose: 1 for Automatic, and while the section is not set or
		// its stored value is refused.
		float GetScaleFactor(const EditorPreferences& preferences);

	}

}
