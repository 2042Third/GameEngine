#pragma once

#include "UI/EditorPanelRegistry.h"
#include "UI/SettingsPages.h"

#include <string>

namespace Strata
{

	// File > Preferences...: the person's preferences (the settings registry's user sections, kept for every project by
	// EditorPreferences) in a modal dialog drawn by EditorLayer, so that it works from the launcher as well, where docked panels
	// are not drawn. Scripts > External Editor... opens it at that section (EditorShell::ShowPreferencesDialog). Changes apply
	// at once (preferences.set, or the page's command); Close or Escape closes it.
	class PreferencesDialog
	{
	public:
		static constexpr const char* c_Popup = "Preferences";

		// Opens the dialog at a section's page (empty: the page shown last, the first one at first), its page made again from
		// the preferences as they are unless the dialog is open already.
		void Open(const std::string& section = {});
		void Draw(EditorPanelContext& context, const EditorSettingsPageRegistry& pages);
		bool IsOpen() const { return m_Open; }
		const std::string& GetSelected() const { return m_Browser.GetSelected(); }
		SettingsPage* GetPage() const { return m_Browser.GetPage(); }
	private:
		SettingsBrowser m_Browser { SettingsWindowKind::Preferences };
		bool m_OpenRequested = false;
		bool m_Open = false;
	};

}
