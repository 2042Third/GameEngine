#include "Panels/ProjectSettingsPanel.h"

#include "Editor/EditorContext.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>

namespace Strata
{

	ProjectSettingsPanel::ProjectSettingsPanel(const EditorSettingsPageRegistry& pages)
		: m_Pages(pages)
	{
	}

	void ProjectSettingsPanel::OnImGuiRender(EditorPanelContext& context)
	{
		if (!context.Context.HasProject())
		{
			UI::CenteredText("ProjectSettings.NoProject", "No project is open: project settings belong to a project.", UI::GetThemeColors().TextSecondary);
			return;
		}
		m_Browser.Draw(context, m_Pages, ImGui::GetContentRegionAvail());
	}

}
