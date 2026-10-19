#include <doctest/doctest.h>

#include "Editor/EditorContext.h"
#include "Editor/EditorWorld.h"
#include "Editor/HarnessEditor.h"
#include "Editor/WorldProjectUtils.h"
#include "EditorLayer.h"
#include "Panels/StreamingPanel.h"
#include "UI/CellMap.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <set>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The editor with the generated 5 x 5 world "Gen" (WorldProject) open, its camera over cell (0, 0), every wanted cell in.
	struct WorldEditor : HarnessEditor
	{
		explicit WorldEditor(const std::string& name)
		{
			std::filesystem::path projectFile;
			{
				WorldProject project(name, 5);
				projectFile = project.Context.GetProject()->GetProjectFile();
				project.Run("project.close", { { "force", true } });
			}
			RunToCompletion("project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
			RunToCompletion("scene.open", { { "scene", UUIDToJson(UUID(c_GenPersistentHandle)) }, { "force", true } });
			REQUIRE(Context().GetWorld());
			LookFrom(glm::vec3(14.0f, 10.0f, 14.0f));
			Settle();
		}

		EditorWorld& World() { return *Context().GetWorld(); }

		void LookFrom(const glm::vec3& position)
		{
			Run("camera.set", { { "position", { position.x, position.y, position.z } }, { "target", { position.x, 0.0f, position.z - 10.0f } } });
		}

		// Frames until no cell loads or unloads.
		void Settle()
		{
			REQUIRE(FramesUntil([&]()
			{
				const WorldStreamingStats stats = World().GetStreamer().GetStats();
				return World().GetWorldAsset() && World().GetStreamer().HasEvaluated() && stats.CellsLoading == 0 && stats.CellsUnloading == 0;
			}));
			Frames(2);
		}

		StreamingPanel& Streaming()
		{
			StreamingPanel* streaming = Layer->GetPanels().Get<StreamingPanel>(EditorPanels::c_Streaming);
			REQUIRE(streaming);
			return *streaming;
		}

		std::string ProbeText(std::string_view key)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
			return item->Text;
		}
	};

}

TEST_SUITE("Editor.UI.World")
{
	TEST_CASE("The cell map draws the world's cells by state, names them on hover and pins them from its menu")
	{
		WorldEditor editor("WorldUICellMap");
		editor.Streaming().RevealWorld();
		editor.Layer->GetPanels().Focus(EditorPanels::c_Streaming);
		editor.Frames(3);
		const CellMap& map = editor.Streaming().GetCellMap();
		REQUIRE(map.GetCells().size() == 25);
		CHECK(map.GetDrawnCellCount() > 0);
		std::set<WorldCellStatus> seen;
		for (const CellMap::Cell& cell : map.GetCells())
			seen.insert(cell.Status);
		CHECK(seen.contains(WorldCellStatus::Loaded));
		CHECK(seen.contains(WorldCellStatus::Unloaded));
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Streaming)) == 0);

		// Moving the camera shows cells loading and unloading on the way.
		editor.LookFrom(glm::vec3(140.0f, 10.0f, 140.0f));
		REQUIRE(editor.FramesUntil([&]()
		{
			for (const CellMap::Cell& cell : map.GetCells())
				seen.insert(cell.Status);
			return seen.contains(WorldCellStatus::Loading) && seen.contains(WorldCellStatus::Unloading);
		}));
		editor.Settle();
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Streaming)) == 0);

		// Hovering the map's center names its cell; a right click there offers Load and pin, which pins it.
		const std::optional<UI::ItemProbe::Item> area = UI::ItemProbe::Find("Streaming.CellMap");
		REQUIRE(area);
		// The map shows whole in the panel's short bottom dock.
		const ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Streaming);
		REQUIRE(window);
		CHECK(area->Max.y <= window->Pos.y + window->Size.y + 1.0f);
		editor.Harness.MoveMouse(area->GetCenter());
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("Streaming.CellMap.Tooltip").has_value());
		CHECK(UI::ItemProbe::Find("Streaming.CellMap.Tooltip")->Text == "Ground");
		editor.Harness.SetMouseButton(ImGuiMouseButton_Right, true);
		editor.Frames(1);
		editor.Harness.SetMouseButton(ImGuiMouseButton_Right, false);
		editor.Frames(2);
		REQUIRE(map.GetSelectedCell().has_value());
		const WorldCellKey selected = *map.GetSelectedCell();
		REQUIRE(editor.Click("Streaming.CellMap.LoadAndPin"));
		CHECK(editor.World().GetPinnedCells().contains(selected));
		editor.Frames(2);
		const CellMap::Cell* pinned = nullptr;
		for (const CellMap::Cell& cell : map.GetCells())
			pinned = cell.Key == selected ? &cell : pinned;
		REQUIRE(pinned);
		CHECK(pinned->Pinned);

		// An edited cell is outlined as unsaved.
		const Scene& scene = *editor.Context().GetEditScene();
		const SubSceneID subScene = editor.World().GetStreamer().GetCellSubScene(selected);
		REQUIRE(subScene != 0);
		const Entity root = scene.GetSubSceneRoots(subScene).front();
		editor.Run("entity.rename", { { "entity", UUIDToJson(root.GetUUID()) }, { "name", "Edited" } });
		editor.Frames(2);
		for (const CellMap::Cell& cell : map.GetCells())
			pinned = cell.Key == selected ? &cell : pinned;
		CHECK(pinned->Dirty);
	}

	TEST_CASE("With the Streaming panel closed, the cell map copies nothing while the world streams")
	{
		WorldEditor editor("WorldUIClosed");
		editor.Streaming().RevealWorld();
		editor.Layer->GetPanels().Focus(EditorPanels::c_Streaming);
		editor.Frames(3);
		const CellMap& map = editor.Streaming().GetCellMap();
		REQUIRE(map.GetRebuildCount() > 0);
		// Still: nothing is copied again.
		uint64_t rebuilds = map.GetRebuildCount();
		editor.Frames(10);
		CHECK(map.GetRebuildCount() == rebuilds);

		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Streaming, false);
		editor.Frames(2);
		rebuilds = map.GetRebuildCount();
		editor.LookFrom(glm::vec3(140.0f, 10.0f, 140.0f));
		editor.Settle();
		CHECK(map.GetRebuildCount() == rebuilds);
	}
}
