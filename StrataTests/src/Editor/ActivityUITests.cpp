#include <doctest/doctest.h>

#include "Editor/CommandJournal.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/UndoStack.h"
#include "Panels/ActivityPanel.h"
#include "Panels/ViewportPanel.h"
#include "TestHelpers.h"
#include "UI/EditorFonts.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/RpcClient.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The Activity panel (the activity journal in the UI) and agent presence: the automation pill, the viewport's agent chip
// and the outline of what an agent changed.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	std::string RowKey(uint64_t id)
	{
		return fmt::format("Activity.Row.{}", id);
	}

	uint64_t FindLatest(const CommandJournal& journal, std::string_view command)
	{
		for (auto it = journal.GetEntries().rbegin(); it != journal.GetEntries().rend(); ++it)
		{
			if (it->Command == command)
				return it->Id;
		}
		return 0;
	}

	// The table the window (or a child window of it) drew last with this many columns.
	ImGuiTable* FindTable(ImGuiWindow* window, int columns)
	{
		ImGuiContext& context = *GImGui;
		for (int index = 0; index < context.Tables.GetMapSize(); index++)
		{
			ImGuiTable* table = context.Tables.TryGetMapData(index);
			if (table && table->ColumnsCount == columns && table->OuterWindow
				&& (table->OuterWindow == window || ImGui::IsWindowChildOf(table->OuterWindow, window, false, false)))
			{
				return table;
			}
		}
		return nullptr;
	}

}

TEST_SUITE("Editor.UI.Activity")
{
	TEST_CASE("The Activity panel lists commands, filters them and acts on the selected one")
	{
		HarnessEditor editor({}, WithFeatureProject("ActivityPanel"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("editor.showPanel", { { "panel", "Activity" } });
		editor.Frames(2);
		ActivityPanel* panel = editor.Layer->GetPanels().Get<ActivityPanel>(EditorPanels::c_Activity);
		REQUIRE(panel);
		REQUIRE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Activity));

		const UndoStack& undo = editor.Context().GetUndoStack();
		const size_t before = undo.GetPosition();
		editor.Run("entity.create", { { "name", "First" } });
		editor.Run("entity.create", { { "name", "Second" } });
		editor.Run("entity.rename", { { "entity", editor.Context().GetEditScene()->FindEntityByName("Second").GetUUID().ToString() }, { "name", "Renamed" } });
		editor.Run("entity.create", { { "name", "Third" } });
		editor.Run("scene.info");
		editor.Frames(2);
		const CommandJournal& journal = editor.Context().GetJournal();
		const uint64_t first = FindLatest(journal, "entity.create") - 3; // The entries follow each other: First's is three before Third's
		REQUIRE(journal.Find(first));
		REQUIRE(journal.Find(first)->Parameters["name"] == "First");
		const std::vector<uint64_t> shown = panel->GetShownEntries();
		CHECK(std::find(shown.begin(), shown.end(), FindLatest(journal, "scene.info")) != shown.end());

		// Filters: text, and only what changed something.
		REQUIRE(editor.ReplaceText("Activity.Search", "renam"));
		editor.Frames(2);
		REQUIRE(panel->GetShownEntries().size() == 1);
		CHECK(journal.Find(panel->GetShownEntries()[0])->Command == "entity.rename");
		REQUIRE(editor.Click("Activity.Search.Clear"));
		REQUIRE(editor.Click("Activity.ChangesOnly"));
		editor.Frames(2);
		for (uint64_t id : panel->GetShownEntries())
			CHECK(journal.Find(id)->Kind == EditorCommandKind::Edit);
		CHECK(std::find(panel->GetShownEntries().begin(), panel->GetShownEntries().end(), FindLatest(journal, "scene.info")) == panel->GetShownEntries().end());

		// The details of an entry (selected as the toasts do it, which scrolls it into view; a click on its row toggles it), and
		// a command script that runs it again.
		panel->Select(first);
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find(RowKey(first)).has_value());
		REQUIRE(editor.Click(RowKey(first)));
		editor.Frames(1);
		CHECK(panel->GetSelected() == 0);
		REQUIRE(editor.Click(RowKey(first)));
		editor.Frames(2);
		CHECK(panel->GetSelected() == first);
		const std::optional<UI::ItemProbe::Item> details = UI::ItemProbe::Find("Activity.Details");
		REQUIRE(details.has_value());
		CHECK(details->Text.find("\"command\": \"entity.create\"") != std::string::npos);
		REQUIRE(editor.Click("Activity.CopyScript"));
		editor.Frames(1);
		const nlohmann::json script = nlohmann::json::parse(editor.Harness.GetClipboard(), nullptr, false);
		REQUIRE(script.is_array());
		REQUIRE(script.size() == 1);
		CHECK(script[0]["command"] == "entity.create");
		CHECK(script[0]["parameters"] == nlohmann::json { { "name", "First" } });

		// Undo to here: back to the step of First, through edit.undo (three steps after it).
		REQUIRE(undo.GetPosition() == before + 4);
		REQUIRE(editor.Click("Activity.UndoToHere"));
		editor.Frames(2);
		CHECK(undo.GetPosition() == before + 1);
		CHECK(undo.GetSerial(undo.GetPosition() - 1) == journal.Find(first)->UndoSerial);
		CHECK(editor.Context().GetEditScene()->FindEntityByName("First"));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Renamed"));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Third"));
		size_t undos = 0;
		for (const CommandJournalEntry& entry : journal.GetEntries())
			undos += entry.Command == "edit.undo" && entry.Source == CommandSource::UI ? 1 : 0;
		CHECK(undos == 3);
		// Its step is the latest now: nothing left to undo to.
		CHECK_FALSE(UI::ItemProbe::Find("Activity.UndoToHere")->Enabled);

		// Re-run: the same command again, from the UI.
		REQUIRE(panel->GetSelected() == first);
		const uint64_t latest = journal.GetLatestId();
		REQUIRE(editor.Click("Activity.Rerun"));
		editor.Frames(2);
		REQUIRE(journal.GetLatestId() > latest);
		const CommandJournalEntry& rerun = journal.GetEntries().back();
		CHECK(rerun.Command == "entity.create");
		CHECK(rerun.Source == CommandSource::UI);
		CHECK(rerun.Parameters == nlohmann::json { { "name", "First" } });
	}

	TEST_CASE("Re-running a command that replaces the scene asks about its unsaved changes again")
	{
		HarnessEditor editor({}, WithFeatureProject("ActivityRerunScene"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("editor.showPanel", { { "panel", "Activity" } });
		editor.Frames(2);
		ActivityPanel* panel = editor.Layer->GetPanels().Get<ActivityPanel>(EditorPanels::c_Activity);
		REQUIRE(panel);

		// The person opened the scene after answering the question about unsaved changes: the entry holds that answer.
		// scene.open answers once the scene is in (it loads in the background): through the runner, as the UI runs it.
		const std::string scene = editor.Context().GetSceneHandle().ToString();
		auto opened = std::make_shared<std::optional<EditorCommandResult>>();
		editor.Layer->GetCommandRunner().Run(editor.Context(), editor.Layer->GetCommands(), "scene.open", { { "scene", scene }, { "force", true } },
			[opened](const EditorCommandResult& result) { *opened = result; }, CommandOrigin { CommandSource::UI });
		REQUIRE(editor.FramesUntil([&]() { return opened->has_value(); }));
		REQUIRE((*opened)->Success);
		const uint64_t entry = (*opened)->JournalId;
		REQUIRE(editor.Context().GetJournal().Find(entry)->Parameters["force"] == true);
		panel->Select(entry);
		editor.Frames(2);

		// As a script, the answer is left out: the script refuses to discard changes unless it says so itself.
		REQUIRE(editor.Click("Activity.CopyScript"));
		editor.Frames(1);
		const nlohmann::json script = nlohmann::json::parse(editor.Harness.GetClipboard(), nullptr, false);
		REQUIRE(script.is_array());
		REQUIRE(script.size() == 1);
		CHECK(script[0]["parameters"] == nlohmann::json { { "scene", scene } });

		// Changes made since are asked about: Cancel keeps them.
		const CommandJournal& journal = editor.Context().GetJournal();
		editor.Run("entity.create", { { "name", "Unsaved" } });
		REQUIRE(editor.Context().IsSceneModified());
		REQUIRE(editor.Click("Activity.Rerun"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnsavedChanges.Cancel").has_value());
		CHECK(FindLatest(journal, "scene.open") == entry);
		REQUIRE(editor.Click("UnsavedChanges.Cancel"));
		editor.Frames(2);
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
		CHECK(FindLatest(journal, "scene.open") == entry);

		// Don't Save runs it again, with the answer.
		REQUIRE(editor.Click("Activity.Rerun"));
		editor.Frames(2);
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		// The scene loads in the background: the run ends once it replaced the edited one.
		REQUIRE(editor.FramesUntil([&]()
		{
			const uint64_t latest = FindLatest(journal, "scene.open");
			return latest > entry && journal.Find(latest)->Status != CommandStatus::Pending;
		}));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
		CHECK_FALSE(editor.Context().IsSceneModified());
		const uint64_t rerunId = FindLatest(journal, "scene.open");
		REQUIRE(rerunId > entry);
		const CommandJournalEntry& rerun = *journal.Find(rerunId);
		CHECK(rerun.Source == CommandSource::UI);
		CHECK(rerun.Status == CommandStatus::Succeeded);
		CHECK(rerun.Parameters["force"] == true);

		// The list says which run threw changes away (in the warning color); the first discarded nothing.
		panel->Select(rerunId);
		editor.Frames(2);
		const std::optional<UI::ItemProbe::Item> discarded = UI::ItemProbe::Find(fmt::format("Activity.Result.{}", rerunId));
		REQUIRE(discarded.has_value());
		CHECK(discarded->Text == "discarded changes");
		CHECK(discarded->Color == UI::ToColorU32(UI::GetThemeColors().Warning));
		const std::optional<UI::ItemProbe::Item> kept = UI::ItemProbe::Find(fmt::format("Activity.Result.{}", entry));
		REQUIRE(kept.has_value());
		CHECK(kept->Text == "done");
	}

	TEST_CASE("Re-run and Undo to here work on the oldest entry of a full journal, which their own commands push out")
	{
		HarnessEditor editor({}, WithFeatureProject("ActivityFullJournal"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("editor.showPanel", { { "panel", "Activity" } });
		editor.Frames(2);
		ActivityPanel* panel = editor.Layer->GetPanels().Get<ActivityPanel>(EditorPanels::c_Activity);
		REQUIRE(panel);
		const CommandJournal& journal = editor.Context().GetJournal();
		const UndoStack& undo = editor.Context().GetUndoStack();

		// Three edits, then lookups until the first edit is the oldest entry of a full ring: every command the buttons run then
		// drops the oldest entry, the one the details pane shows.
		const uint64_t oldest = editor.Layer->GetCommands().Execute(editor.Context(), "entity.create", { { "name", "Oldest" } }).JournalId;
		editor.Run("entity.create", { { "name", "Second" } });
		editor.Run("entity.create", { { "name", "Third" } });
		REQUIRE(journal.Find(oldest));
		const size_t oldestPosition = undo.GetPosition() - 2;
		while (journal.GetEntries().front().Id < oldest)
			editor.Layer->GetCommands().Execute(editor.Context(), "entity.find", { { "name", "Oldest" } });
		REQUIRE(journal.GetEntries().size() == CommandJournal::c_MaxEntries);
		panel->Select(oldest);
		editor.Frames(2);
		REQUIRE(journal.GetEntries().front().Id == oldest);
		REQUIRE(panel->GetSelected() == oldest);

		// Undo to here: its first edit.undo pushes the entry out while its details are drawn.
		REQUIRE(UI::ItemProbe::Find("Activity.UndoToHere")->Enabled);
		REQUIRE(editor.Click("Activity.UndoToHere"));
		editor.Frames(2);
		CHECK(undo.GetPosition() == oldestPosition);
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Oldest"));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Second"));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Third"));
		CHECK_FALSE(journal.Find(oldest));
		CHECK(panel->GetSelected() == 0); // Gone from the journal, so no longer selected

		// Re-run of the oldest entry now (the third edit; the undos pushed out the first two), which its own run pushes out.
		const uint64_t third = journal.GetEntries().front().Id;
		REQUIRE(journal.GetEntries().front().Command == "entity.create");
		REQUIRE(journal.GetEntries().front().Parameters == nlohmann::json { { "name", "Third" } });
		panel->Select(third);
		editor.Frames(2);
		REQUIRE(journal.GetEntries().front().Id == third);
		REQUIRE(editor.Click("Activity.Rerun"));
		editor.Frames(2);
		CHECK_FALSE(journal.Find(third));
		const CommandJournalEntry& rerun = journal.GetEntries().back();
		CHECK(rerun.Command == "entity.create");
		CHECK(rerun.Source == CommandSource::UI);
		CHECK(rerun.Parameters == nlohmann::json { { "name", "Third" } });
		CHECK(rerun.Status == CommandStatus::Succeeded);
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Third"));
	}

	TEST_CASE("The Activity panel asked for before the first frame joins the bottom area")
	{
		// As a command script's first command (editor.showPanel) does it: before the layout and the Console exist.
		HarnessEditor editor({}, WithFeatureProject("ActivityFirstFrame"));
		editor.Layer->GetPanels().Focus(EditorPanels::c_Activity);
		editor.Frames(3);
		ImGuiWindow* activity = editor.FindPanelWindow(EditorPanels::c_Activity);
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(activity);
		REQUIRE(console);
		CHECK(activity->DockNode != nullptr);
		CHECK(activity->DockNode == console->DockNode);
		CHECK_FALSE(activity->Hidden);
	}

	TEST_CASE("The Activity panel shows results whole in the default layout, and gives parameters what results leave")
	{
		// A maximized window on a 4K display at 150% and at 100%, and a 1600x900 window at 150%, with nothing selected and with
		// the details pane of the selected entry beside the list: a fixed Result column, and then one of two fifths of the width
		// parameters and results share, cut "Rename 'Wall' to 'East Wall'" or the error of a failed command, while parameters,
		// cut in the list anyway, took the rest.
		const ImGuiHarness::Specification windows[] = { { ImVec2(3840.0f, 2054.0f), 1.5f }, { ImVec2(3840.0f, 2054.0f), 1.0f },
			{ ImVec2(2400.0f, 1350.0f), 1.5f } };
		for (const ImGuiHarness::Specification& window : windows)
		{
			CAPTURE(window.DisplaySize.x);
			CAPTURE(window.ContentScale);
			HarnessEditor editor(window, WithFeatureProject("ActivityResultWidth"));
			editor.Frames(2);
			REQUIRE(editor.Context().HasProject());
			editor.Run("editor.showPanel", { { "panel", "Activity" } });
			editor.Frames(3);
			ActivityPanel* panel = editor.Layer->GetPanels().Get<ActivityPanel>(EditorPanels::c_Activity);
			REQUIRE(panel);
			const CommandJournal& journal = editor.Context().GetJournal();
			auto findTable = [&editor]() { return FindTable(editor.FindPanelWindow(EditorPanels::c_Activity), 5); };

			// Only short results ("done"): parameters take most of the shared width.
			ImGuiTable* table = findTable();
			REQUIRE(table);
			CHECK(table->Columns[2].WidthGiven > 2.0f * table->Columns[3].WidthGiven);

			editor.Run("entity.create", { { "name", "Wall" } });
			const UUID wall = editor.Context().GetEditScene()->FindEntityByName("Wall").GetUUID();
			editor.Run("entity.rename", { { "entity", wall.ToString() }, { "name", "East Wall" } });
			const EditorCommandResult failed = editor.Layer->GetCommands().Execute(editor.Context(), "entity.rename",
				{ { "entity", UUID().ToString() }, { "name", "Somewhere Else Entirely" } });
			REQUIRE_FALSE(failed.Success);
			const uint64_t rename = FindLatest(journal, "entity.rename") - 1;
			const uint64_t failure = FindLatest(journal, "entity.rename");
			REQUIRE(journal.Find(rename)->Status == CommandStatus::Succeeded);
			REQUIRE(journal.Find(failure)->Status == CommandStatus::Failed);
			for (const uint64_t selected : { uint64_t(0), rename })
			{
				CAPTURE(selected);
				panel->Select(selected);
				editor.Frames(3);
				table = findTable();
				REQUIRE(table);
				const std::optional<UI::ItemProbe::Item> result = UI::ItemProbe::Find(fmt::format("Activity.Result.{}", rename));
				const std::optional<UI::ItemProbe::Item> error = UI::ItemProbe::Find(fmt::format("Activity.Result.{}", failure));
				REQUIRE(result);
				REQUIRE(error);
				CHECK(result->Text == "Rename 'Wall' to 'East Wall'");
				CHECK_FALSE(error->Text.empty());
				// The texts end within their column (an item's rectangle is the whole text, whatever the column clips), and the
				// parameters keep a share of the width.
				CHECK(result->Max.x <= table->Columns[3].ClipRect.Max.x);
				// The error is longer: whole wherever the list leaves parameters their least (here, all but the narrowest
				// window with the details pane beside the list).
				if (selected == 0 || window.DisplaySize.x > 3000.0f)
					CHECK(error->Max.x <= table->Columns[3].ClipRect.Max.x);
				CHECK(table->Columns[2].WidthGiven >= 7.5f * ImGui::GetFontSize());
			}
		}
	}

	TEST_CASE("Results in Chinese, Japanese and Korean are measured again once the system's fonts for them are merged")
	{
		// An undo step naming an entity in Han characters, listed before the system's fonts were read: measured then, each of
		// them was ImGui's fallback character, half as wide. The Result column kept that width and cut the text.
		HarnessEditor editor({ ImVec2(3840.0f, 2054.0f), 1.0f }, WithFeatureProject("ActivityFallbackFonts"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		REQUIRE_FALSE(UI::EditorFonts::HasFallback());
		// "Rename 'Wall' to '東壁西壁北壁南壁大門'": Han characters only, which every system font for these scripts has.
		const std::string name = "\xE6\x9D\xB1\xE5\xA3\x81\xE8\xA5\xBF\xE5\xA3\x81\xE5\x8C\x97\xE5\xA3\x81\xE5\x8D\x97\xE5\xA3\x81"
								 "\xE5\xA4\xA7\xE9\x96\x80";
		editor.Run("entity.create", { { "name", "Wall" } });
		const UUID wall = editor.Context().GetEditScene()->FindEntityByName("Wall").GetUUID();
		editor.Run("entity.rename", { { "entity", wall.ToString() }, { "name", name } });
		editor.Run("editor.showPanel", { { "panel", "Activity" } });
		editor.Frames(3);
		const uint64_t rename = FindLatest(editor.Context().GetJournal(), "entity.rename");
		const std::string resultKey = fmt::format("Activity.Result.{}", rename);
		std::optional<UI::ItemProbe::Item> result = UI::ItemProbe::Find(resultKey);
		REQUIRE(result);
		const float widthBefore = result->Max.x - result->Min.x;
		// The name's width in the font the Result column draws with (the UI's), with the fonts the editor has at the time.
		const auto nameWidth = [&editor, &name]()
		{
			float width = 0.0f;
			editor.FrameWith([&width, &name]() { width = ImGui::CalcTextSize(name.c_str()).x; });
			return width;
		};
		const float nameBefore = nameWidth();

		UI::EditorFonts::BeginLoadingFallback();
		REQUIRE(editor.FramesUntil([]() { return !UI::EditorFonts::IsLoadingFallback(); }));
		editor.Frames(3);
		if (Platform::FindFallbackFontFiles().empty())
		{
			MESSAGE("This system has no fonts for Chinese, Japanese and Korean text");
			return;
		}
		REQUIRE(UI::EditorFonts::HasFallback());
		result = UI::ItemProbe::Find(resultKey);
		REQUIRE(result);
		ImGuiTable* table = FindTable(editor.FindPanelWindow(EditorPanels::c_Activity), 5);
		REQUIRE(table);
		// Wider now by what the merged glyphs add to the name (as much as the system's font makes them wider than ImGui's
		// fallback character), and still whole in its column.
		const float growth = nameWidth() - nameBefore;
		CHECK(growth > 0.0f);
		// Item rectangles are whole pixels: within one of the measured growth.
		CHECK(std::abs(result->Max.x - result->Min.x - (widthBefore + growth)) <= 1.0f);
		CHECK(result->Max.x <= table->Columns[3].ClipRect.Max.x);
	}

	TEST_CASE("A panel asked for while a dialog waits for the person opens behind it, and comes forward after")
	{
		HarnessEditor editor({}, WithFeatureProject("ActivityBehindDialog"));
		editor.Frames(2);
		editor.Run("editor.showDialog", { { "dialog", "about" } });
		editor.Frames(2);
		REQUIRE(editor.Layer->GetAboutDialog().IsOpen());
		// An agent shows the Activity panel meanwhile: the dialog stays.
		editor.Run("editor.showPanel", { { "panel", "Activity" } });
		editor.Frames(3);
		CHECK(editor.Layer->GetAboutDialog().IsOpen());
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Activity));
		// Once the person closes the dialog, the panel comes to the front.
		REQUIRE(editor.Click("About.Close"));
		editor.Frames(3);
		CHECK_FALSE(editor.Layer->GetAboutDialog().IsOpen());
		ImGuiWindow* activity = editor.FindPanelWindow(EditorPanels::c_Activity);
		REQUIRE(activity);
		CHECK_FALSE(activity->Hidden);
	}

	TEST_CASE("An agent shows in the automation pill, a viewport chip and an outline of what it changed")
	{
		const std::filesystem::path sessions = CreateTemporaryDirectory("ActivityPresenceSessions");
		ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessions));
		// The time the viewport's clock gives once the test sets it (below): declared before the editor, so that it outlives
		// the clock also when a check ends the test early.
		std::optional<std::chrono::steady_clock::time_point> now;
		HarnessEditor editor({}, WithFeatureProject("ActivityPresence"), {}, true);
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		CHECK(editor.GetPillText("Status.Automation").find("no agent") != std::string::npos);
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.Agent").has_value());

		const std::vector<EditorSessionInfo> found = EditorSession::FindSessions(sessions);
		REQUIRE(found.size() == 1);
		RpcClient client;
		REQUIRE_MESSAGE(client.Connect(found[0].Address, found[0].Port, found[0].Token, std::chrono::milliseconds(5000), "Claude Code"), client.GetLastError());
		REQUIRE(editor.FramesUntil([&editor]() { return editor.GetPillText("Status.Automation") == "Agent connected \xC2\xB7 Claude Code"; }));

		// The viewport goes by a clock of the test's, so that what it shows does not depend on how long frames take: the real
		// one until the command finished, then times after that.
		ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		viewport->SetClock([&now]() { return now.value_or(std::chrono::steady_clock::now()); });

		// The agent makes a cube in front of the editor camera.
		const nlohmann::json camera = editor.Run("camera.get");
		std::future<RpcResult> call = std::async(std::launch::async, [&client, &camera]()
		{
			return client.Call("entity.create", { { "name", "AgentCube" }, { "components", { { "MeshRenderer", { { "Mesh", "Builtin/Cube" } } },
				{ "Transform", { { "Translation", camera["target"] } } } } } }, std::chrono::milliseconds(10000));
		});
		REQUIRE(editor.FramesUntil([&call]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
		REQUIRE(call.get().IsSuccess());
		// A query the agent runs afterwards does not replace it: the chip names what changed the scene.
		std::future<RpcResult> query = std::async(std::launch::async, [&client]()
		{
			return client.Call("editor.status", nlohmann::json::object(), std::chrono::milliseconds(10000));
		});
		REQUIRE(editor.FramesUntil([&query]() { return query.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
		REQUIRE(query.get().IsSuccess());
		const AutomationActivity& latest = editor.Context().GetJournal().GetLatestAutomationActivity();
		REQUIRE(latest.Command == "entity.create");
		const auto finished = latest.Finished;
		const auto at = [finished](double seconds)
		{
			return finished + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
		};
		now = at(ViewportPanel::c_AgentFlashSeconds * 0.5);
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> chip = UI::ItemProbe::Find("Viewport.Agent");
		REQUIRE(chip.has_value());
		CHECK(chip->Text.rfind("Agent: entity.create \xC2\xB7 ", 0) == 0);
		const std::optional<UI::ItemProbe::Item> flash = UI::ItemProbe::Find("Viewport.AgentFlash");
		REQUIRE(flash.has_value());
		CHECK(flash->Max.x > flash->Min.x);
		CHECK(flash->Max.y > flash->Min.y);
		CHECK(viewport->IsAnimating()); // The fade runs at the full frame rate
		CHECK(viewport->IsFlashing(at(ViewportPanel::c_AgentFlashSeconds - 0.01)));

		// The outline fades within c_AgentFlashSeconds; the chip stays while the agent is connected. The fade ends by the clock,
		// also for a viewport that is not drawn meanwhile (hidden behind another tab), which must not keep the editor at its
		// full frame rate.
		CHECK_FALSE(viewport->IsFlashing(at(ViewportPanel::c_AgentFlashSeconds + 0.01)));
		now = at(ViewportPanel::c_AgentFlashSeconds + 0.1);
		CHECK_FALSE(viewport->IsAnimating());
		editor.Frames(1);
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.AgentFlash").has_value());
		CHECK_FALSE(viewport->IsAnimating());
		REQUIRE(UI::ItemProbe::Find("Viewport.Agent").has_value());
		// A minute later, while the agent is still connected, the chip says how long ago that was.
		now = at(61.0);
		editor.Frames(1);
		REQUIRE(UI::ItemProbe::Find("Viewport.Agent").has_value());
		CHECK(UI::ItemProbe::Find("Viewport.Agent")->Text.find("1 min") != std::string::npos);
		viewport->SetClock(nullptr);

		// A click on the chip shows the command in the Activity panel.
		REQUIRE(editor.Click("Viewport.Agent"));
		editor.Frames(2);
		ActivityPanel* activity = editor.Layer->GetPanels().Get<ActivityPanel>(EditorPanels::c_Activity);
		REQUIRE(activity);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Activity));
		CHECK(activity->GetSelected() == editor.Context().GetJournal().GetLatestAutomationActivity().Id);
		client.Close();
	}
}
