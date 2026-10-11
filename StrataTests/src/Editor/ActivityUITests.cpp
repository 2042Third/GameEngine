#include <doctest/doctest.h>

#include "Editor/CommandJournal.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/UndoStack.h"
#include "Panels/ActivityPanel.h"
#include "Panels/ViewportPanel.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/RpcClient.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <string>
#include <thread>
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

	TEST_CASE("An agent shows in the automation pill, a viewport chip and an outline of what it changed")
	{
		const std::filesystem::path sessions = CreateTemporaryDirectory("ActivityPresenceSessions");
		ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessions));
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

		// The agent makes a cube in front of the editor camera.
		const nlohmann::json camera = editor.Run("camera.get");
		std::future<RpcResult> call = std::async(std::launch::async, [&client, &camera]()
		{
			return client.Call("entity.create", { { "name", "AgentCube" }, { "components", { { "MeshRenderer", { { "Mesh", "Builtin/Cube" } } },
				{ "Transform", { { "Translation", camera["target"] } } } } } }, std::chrono::milliseconds(10000));
		});
		REQUIRE(editor.FramesUntil([&call]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
		REQUIRE(call.get().IsSuccess());
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> chip = UI::ItemProbe::Find("Viewport.Agent");
		REQUIRE(chip.has_value());
		CHECK(chip->Text.rfind("Agent: entity.create \xC2\xB7 ", 0) == 0);
		const std::optional<UI::ItemProbe::Item> flash = UI::ItemProbe::Find("Viewport.AgentFlash");
		REQUIRE(flash.has_value());
		CHECK(flash->Max.x > flash->Min.x);
		CHECK(flash->Max.y > flash->Min.y);
		ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		CHECK(viewport->IsAnimating()); // The fade runs at the full frame rate

		// The outline fades within c_AgentFlashSeconds; the chip stays while the agent is connected.
		std::this_thread::sleep_for(std::chrono::duration<double>(ViewportPanel::c_AgentFlashSeconds + 0.1));
		editor.Frames(1);
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.AgentFlash").has_value());
		CHECK_FALSE(viewport->IsAnimating());
		REQUIRE(UI::ItemProbe::Find("Viewport.Agent").has_value());

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
