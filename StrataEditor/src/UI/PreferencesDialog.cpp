#include "UI/PreferencesDialog.h"

#include "UI/ItemProbe.h"
#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		// The dialog's content, in text heights: room for the list of pages and a page of fields.
		constexpr float c_WidthInFontSizes = 46.0f;
		constexpr float c_HeightInFontSizes = 26.0f;

	}

	void PreferencesDialog::Open(const std::string& section)
	{
		if (!section.empty())
			m_Browser.Select(section);
		// Opening again starts from the preferences as they are: a command typed but not applied before the dialog closed is
		// gone, and the editors on the machine are looked for again.
		if (!m_Open)
			m_Browser.DiscardPage();
		m_OpenRequested = true;
	}

	void PreferencesDialog::Draw(EditorPanelContext& context, const EditorSettingsPageRegistry& pages)
	{
		if (m_OpenRequested)
		{
			UI::OpenModal(c_Popup);
			m_OpenRequested = false;
		}
		bool open = true;
		m_Open = UI::BeginModal(c_Popup, "Preferences", &open);
		if (!m_Open)
			return;
		// Escape leaves a field first, then closes the dialog.
		const bool editing = ImGui::IsAnyItemActive();
		const float fontSize = ImGui::GetFontSize();
		const ImVec2 size(fontSize * c_WidthInFontSizes, fontSize * c_HeightInFontSizes);
		UI::ItemProbe::Record("Preferences.Dialog");
		m_Browser.Draw(context, pages, size);

		ImGui::Spacing();
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(size.x - UI::DialogButtonWidth("Close"), 0.0f));
		const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (UI::DialogButton("Preferences.Close", "Close", true) || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !editing && focused))
			open = false;
		if (!open)
			ImGui::CloseCurrentPopup();
		UI::EndModal();
	}

}
