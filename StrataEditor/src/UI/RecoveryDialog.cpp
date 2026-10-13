#include "UI/RecoveryDialog.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneRecovery.h"
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
#include <string>
#include <vector>

namespace Strata
{

	namespace
	{

		// The dialog's content width, in text heights.
		constexpr float c_WidthInFontSizes = 34.0f;
		// Up to this many entries the dialog grows with them; more scroll in a list of this height, in text heights (about
		// four and a half entries, so that a cut one shows there are more).
		constexpr size_t c_MaxEntriesWithoutScrolling = 4;
		constexpr float c_ListHeightInFontSizes = 26.0f;

		int64_t GetUnixSeconds()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

	}

	bool RecoveryDialog::HasOffer(const EditorContext& context, bool leftBefore)
	{
		const SceneRecovery& recovery = context.GetRecovery();
		const std::vector<RecoveryEntry>& entries = recovery.GetEntries();
		return context.HasProject() && std::any_of(entries.begin(), entries.end(), [&recovery, leftBefore](const RecoveryEntry& entry)
		{
			return entry.Applicable && (!leftBefore || !recovery.WasDiscardedHere(entry.Id));
		});
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
			}, CommandOrigin { CommandSource::UI });
	}

	void RecoveryDialog::Draw(EditorPanelContext& context)
	{
		const SceneRecovery& recovery = context.Context.GetRecovery();
		// Once per opened project, when its scan has found changes that still apply and that this editor did not discard itself
		// (a command script that discards changes in the editor's first frames: its toast offers them already).
		const Ref<Project>& project = context.Context.GetProject();
		if (project != m_OfferedFor.lock() && !recovery.IsScanning())
		{
			m_OfferedFor = project;
			if (HasOffer(context.Context, true))
				m_OpenRequested = true;
		}
		// Opened once a scan running (e.g. after a discard just now) has read the directory: the dialog appears with its entries,
		// centered for its full size, instead of growing from "nothing yet" past where it was placed.
		if (m_OpenRequested && !recovery.IsScanning())
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
		const bool sceneLoading = context.Context.IsSceneLoading();
		// Many entries scroll in a list of a fixed height instead of growing the dialog past the window. Fixed, because the
		// dialog sizes itself to its contents when it appears (and is centered for that size): a list that measured itself
		// only in its second frame would leave it off center.
		const bool scrolls = entries.size() > c_MaxEntriesWithoutScrolling;
		bool listVisible = true;
		if (scrolls)
			listVisible = ImGui::BeginChild("Entries", ImVec2(width, ImGui::GetFontSize() * c_ListHeightInFontSizes));
		// Wrapped at the list's edge, which leaves room for its scroll bar.
		const float wrapWidth = scrolls ? ImGui::GetContentRegionAvail().x : width;
		for (size_t index = 0; listVisible && index < entries.size(); index++)
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
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
			ImGui::TextColored(colors.TextSecondary, "%s", details.c_str());
			if (!entry.Applicable)
				ImGui::TextColored(colors.Warning, "%s", entry.Problem.c_str());
			ImGui::PopTextWrapPos();

			UI::ButtonStyle recover;
			recover.Primary = entry.Applicable;
			// A scene being opened (the project's start scene, as the dialog appears) would replace what is recovered.
			recover.Enabled = entry.Applicable && idle && !sceneLoading;
			const std::string recoverId = fmt::format("Recovery.Recover.{}", index);
			const char* recoverTooltip = sceneLoading ? "Available once the scene being opened is in"
				: "Open the scene with these changes (unsaved; undo returns to the saved scene)";
			if (UI::ActionButton(recoverId.c_str(), Icons::ArchiveRestore, "Recover", recoverTooltip, recover))
			{
				// Recovering replaces the edited scene: its own unsaved changes are asked about first, over the dialog
				// (DrawDiscardQuestion below). The answer may come in a later frame, so the action keeps what outlives this one.
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
		if (scrolls)
			ImGui::EndChild();

		if (!m_Error.empty())
		{
			ImGui::Spacing();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
			ImGui::TextColored(colors.Error, "%s", m_Error.c_str());
			ImGui::PopTextWrapPos();
		}
		ImGui::Spacing();
		ImGui::Dummy(ImVec2(width, 0.0f));
		const float rowStart = ImGui::GetCursorPosX();
		// Several entries (an agent that discarded scene after scene) go in one click; each is kept in Recovery/Discarded.
		if (entries.size() > 1)
		{
			if (UI::DialogButton("Recovery.DiscardAll", "Discard All", false, idle))
			{
				std::vector<std::string> ids;
				ids.reserve(entries.size());
				for (const RecoveryEntry& entry : entries)
					ids.push_back(entry.Id);
				for (const std::string& id : ids)
					Run(context.Context, context.Commands, context.Runner, id, "discard", false);
			}
			ImGui::SameLine();
		}
		ImGui::SetCursorPosX(rowStart + width - UI::DialogButtonWidth("Later"));
		// Escape belongs to the question over the dialog while that shows.
		const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false) && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		const bool later = UI::DialogButton("Recovery.Later", "Later");
		// Recover asks about the edited scene's own unsaved changes over the dialog, which stays open meanwhile.
		if (context.Shell)
			context.Shell->DrawDiscardQuestion();
		if (later || !open || (escape && idle))
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		// A recovered scene is what the person wanted to see: the dialog closes once nothing is left that applies (known only
		// once a scan running, e.g. after a discard just now, has read the directory).
		else if (idle && !recovery.IsScanning() && !HasOffer(context.Context) && m_Error.empty())
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		UI::EndModal();
	}

}
