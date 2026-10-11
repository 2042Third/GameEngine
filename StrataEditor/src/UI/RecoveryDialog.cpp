#include "UI/RecoveryDialog.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorContext.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Project/Project.h>

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>

namespace Strata
{

	namespace
	{

		// The dialog's content width, in text heights.
		constexpr float c_WidthInFontSizes = 34.0f;

		int64_t GetUnixSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

	}

	bool RecoveryDialog::HasOffer(const EditorContext& context)
	{
		const std::vector<RecoveryEntry>& entries = context.GetRecovery().GetEntries();
		return context.HasProject() && std::any_of(entries.begin(), entries.end(), [](const RecoveryEntry& entry) { return entry.Applicable; });
	}

	void RecoveryDialog::Run(EditorContext& context, const EditorCommandRegistry& commands, EditorCommandRunner& runner, const std::string& id,
		const char* action, bool force)
	{
		m_Error.clear();
		m_Running++;
		runner.Run(context, commands, "scene.recover", { { "scene", id }, { "action", action }, { "force", force } },
			[this](const EditorCommandResult& result)
			{
				m_Running--;
				if (!result.Success)
					m_Error = result.Error;
			});
	}

	void RecoveryDialog::Draw(EditorPanelContext& context)
	{
		const SceneRecovery& recovery = context.Context.GetRecovery();
		// Once per opened project, when its scan has found changes that still apply.
		const Ref<Project>& project = context.Context.GetProject();
		if (project != m_OfferedFor.lock() && !recovery.IsScanning())
		{
			m_OfferedFor = project;
			if (HasOffer(context.Context))
				m_OpenRequested = true;
		}
		if (m_OpenRequested)
		{
			m_OpenRequested = false;
			m_Error.clear();
			UI::OpenModal(c_Popup);
		}
		bool open = true;
		m_Open = UI::BeginModal(c_Popup, "Recover Unsaved Changes", &open);
		if (!m_Open)
			return;

		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float width = ImGui::GetFontSize() * c_WidthInFontSizes;
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
		ImGui::TextColored(colors.TextSecondary, "%s", "These scene changes were not saved. Recover them to continue where the editing stopped (the scene stays "
			"unsaved, and undo returns to its saved state), or discard them.");
		ImGui::PopTextWrapPos();
		ImGui::Spacing();

		const std::vector<RecoveryEntry>& entries = recovery.GetEntries();
		if (entries.empty())
			ImGui::TextColored(colors.TextSecondary, "%s", recovery.IsScanning() ? "Looking for recovery data..." : "Nothing left to recover.");
		const int64_t now = GetUnixSeconds();
		const bool idle = m_Running == 0;
		for (size_t index = 0; index < entries.size(); index++)
		{
			const RecoveryEntry& entry = entries[index];
			ImGui::PushID(static_cast<int>(index));
			ImGui::Separator();
			UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
			ImGui::TextUnformatted(entry.SceneName.c_str());
			ImGui::PopFont();
			const std::string where = entry.ScenePath.empty() ? std::string("never saved") : entry.ScenePath;
			const std::string details = fmt::format("{} \xC2\xB7 {} {} \xC2\xB7 {} \xC2\xB7 {}", where, entry.EntityCount, entry.EntityCount == 1 ? "entity" : "entities",
				UI::DescribeTimeAgo(entry.Time / 1000, now), entry.Discarded ? "discarded on purpose" : "left when the editor closed unexpectedly");
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextColored(colors.TextSecondary, "%s", details.c_str());
			if (!entry.Applicable)
				ImGui::TextColored(colors.Warning, "%s", entry.Problem.c_str());
			ImGui::PopTextWrapPos();

			UI::ButtonStyle recover;
			recover.Primary = entry.Applicable;
			recover.Enabled = entry.Applicable && idle;
			const std::string recoverId = fmt::format("Recovery.Recover.{}", index);
			if (UI::ActionButton(recoverId.c_str(), Icons::ArchiveRestore, "Recover", "Open the scene with these changes (unsaved; undo returns to the saved scene)",
				recover))
			{
				// Recovering replaces the edited scene: its own unsaved changes are asked about first. The answer may come in a
				// later frame, so the action keeps what outlives this one.
				EditorContext* editor = &context.Context;
				const EditorCommandRegistry* commands = &context.Commands;
				EditorCommandRunner* runner = &context.Runner;
				const std::string id = entry.Id;
				if (context.Shell)
					context.Shell->RequestDiscardChanges([this, editor, commands, runner, id]() { Run(*editor, *commands, *runner, id, "apply", true); });
				else
					Run(*editor, *commands, *runner, id, "apply", false);
			}
			ImGui::SameLine();
			UI::ButtonStyle discard;
			discard.Enabled = idle;
			const std::string discardId = fmt::format("Recovery.Discard.{}", index);
			if (UI::ActionButton(discardId.c_str(), Icons::Trash2, "Discard", "Do not offer these changes again (they are kept in .strata/Recovery/Discarded for a while)",
				discard))
			{
				Run(context.Context, context.Commands, context.Runner, entry.Id, "discard", false);
			}
			ImGui::PopID();
		}

		if (!m_Error.empty())
		{
			ImGui::Spacing();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextColored(colors.Error, "%s", m_Error.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::Spacing();
		ImGui::Dummy(ImVec2(width, 0.0f));
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width - UI::DialogButtonWidth("Later"));
		if (UI::DialogButton("Recovery.Later", "Later") || !open || (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && idle))
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		// A recovered scene is what the person wanted to see: the dialog closes once nothing is left that applies.
		else if (idle && !HasOffer(context.Context) && m_Error.empty())
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		UI::EndModal();
	}

}
