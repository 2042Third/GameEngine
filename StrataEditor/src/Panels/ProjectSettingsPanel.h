#pragma once

#include "UI/EditorPanelRegistry.h"
#include "UI/SettingsPages.h"

#include <string>
#include <utility>

namespace Strata
{

	// The Project Settings panel (File > Project Settings..., editor.showPanel ProjectSettings): the project's own settings
	// (the Game and Export pages) and the project settings sections the engine's modules and the game register, each a page
	// that runs its commands. A docked panel rather than a dialog, so that it can stay open while the game plays (changes of
	// the project's settings reach the running game). Not open by default.
	class ProjectSettingsPanel final : public EditorPanel
	{
	public:
		// `pages` (the editor's custom pages) outlives the panel.
		explicit ProjectSettingsPanel(const EditorSettingsPageRegistry& pages);

		void OnImGuiRender(EditorPanelContext& context) override;
		// Shows a page from the next frame on (SettingsBrowser::Select).
		void Select(std::string page) { m_Browser.Select(std::move(page)); }
	private:
		const EditorSettingsPageRegistry& m_Pages;
		SettingsBrowser m_Browser { SettingsWindowKind::Project };
	};

}
