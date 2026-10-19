#include <doctest/doctest.h>

#include "Editor/EditorContext.h"
#include "Editor/EditorWorld.h"
#include "Editor/HarnessEditor.h"
#include "Editor/WorldProjectUtils.h"
#include "EditorLayer.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/SceneHierarchyPanel.h"
#include "Panels/StreamingPanel.h"
#include "UI/CellMap.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/World/WorldAsset.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The editor with the generated 5 x 5 world "Gen" (GridWorldProject) open, its camera over cell (0, 0), every wanted cell in.
	struct WorldEditor : HarnessEditor
	{
		explicit WorldEditor(const std::string& name)
		{
			std::filesystem::path projectFile;
			{
				GridWorldProject project(name, 5);
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
	TEST_CASE("The Hierarchy's world strip names the world, its combo runs world.edit and its button opens the cell map")
	{
		WorldEditor editor("WorldUIStrip");
		CHECK(editor.ProbeText("Hierarchy.WorldName") == "Gen");
		CHECK(editor.ProbeText("Hierarchy.EditLayer") == "Persistent");
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Hierarchy)) == 0);

		REQUIRE(editor.Click("Hierarchy.EditLayer"));
		editor.Frames(2);
		REQUIRE(editor.Click("Hierarchy.EditLayer.Ground"));
		CHECK(editor.World().GetEditLayer() == "Ground");
		editor.Frames(1);
		CHECK(editor.ProbeText("Hierarchy.EditLayer") == "Ground");

		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		REQUIRE(editor.Click("Hierarchy.CellMap"));
		editor.Frames(3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		CHECK(UI::ItemProbe::Find("Streaming.CellMap").has_value());

		// Playing hides the strip: the edit layer is for editing.
		editor.Run("play.start");
		editor.Frames(3);
		CHECK_FALSE(UI::ItemProbe::Find("Hierarchy.WorldName").has_value());
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("Hierarchy.WorldName").has_value());
	}

	TEST_CASE("The Hierarchy's l: and cell: terms find a cell's entities, and the Inspector names the cell")
	{
		WorldEditor editor("WorldUIFilter");
		const SceneHierarchyPanel* hierarchy = editor.Layer->GetPanels().Get<SceneHierarchyPanel>(EditorPanels::c_Hierarchy);
		REQUIRE(hierarchy);
		// Cell (0, 0) holds Box 0 to Box 6 (7 entities).
		REQUIRE(editor.ReplaceText("Hierarchy.Search", "cell:0,0"));
		editor.Frames(2);
		CHECK(hierarchy->GetView().GetRows().size() == 7);
		REQUIRE(editor.ReplaceText("Hierarchy.Search", "l:persistent"));
		editor.Frames(2);
		std::set<std::string> persistent;
		for (const HierarchyRow& row : hierarchy->GetView().GetRows())
			persistent.insert(editor.Context().GetEditScene()->GetEntityByUUID(row.ID).GetName());
		CHECK(persistent == std::set<std::string> { "World", "Source", "Floor" });
		REQUIRE(editor.ReplaceText("Hierarchy.Search", "l:ground box"));
		editor.Frames(2);
		CHECK(hierarchy->GetView().GetRows().size() > 7);

		// Agents fill the search for the person (editor.showPanel's search), as if typed; only the Hierarchy takes one.
		CHECK(editor.Run("editor.showPanel", { { "panel", "Hierarchy" }, { "search", "cell:0,0" } })["search"] == "cell:0,0");
		editor.Frames(2);
		CHECK(hierarchy->GetView().GetFilterText() == "cell:0,0");
		CHECK(hierarchy->GetView().GetRows().size() == 7);
		const EditorCommandResult refused = editor.Layer->GetCommands().Execute(editor.Context(), "editor.showPanel", { { "panel", "Console" }, { "search", "x" } });
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		editor.Run("editor.showPanel", { { "panel", "Hierarchy" }, { "search", "" } });
		editor.Frames(2);
		CHECK_FALSE(hierarchy->GetView().IsFiltering());

		// The Inspector's header names the selected entity's cell, or the persistent scene.
		const Scene& scene = *editor.Context().GetEditScene();
		Entity box;
		for (const SubSceneStatus& status : scene.GetSubScenes())
		{
			if (status.Scene == DeriveSubAssetHandle(UUID(c_GenWorldHandle), "Cell/1/0/0"))
				box = scene.GetSubSceneRoots(status.ID).front();
		}
		REQUIRE(box);
		editor.Run("selection.set", { { "entities", { UUIDToJson(box.GetUUID()) } } });
		editor.Frames(2);
		CHECK(editor.ProbeText("Inspector.Cell") == "Cell: Ground (0, 0)");
		editor.Run("selection.set", { { "entities", { UUIDToJson(scene.FindEntityByName("Source").GetUUID()) } } });
		editor.Frames(2);
		CHECK(editor.ProbeText("Inspector.Cell") == "Persistent scene");
	}

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

	TEST_CASE("The Asset panel shows a World asset's layers, cooks it and opens it")
	{
		WorldEditor editor("WorldUIAsset");
		const AssetHandle world = UUID(c_GenWorldHandle);
		REQUIRE(editor.Context().SetInspectedAsset(world));
		editor.Layer->GetPanels().Focus(EditorPanels::c_AssetInspector);
		REQUIRE(editor.FramesUntil([]() { return UI::ItemProbe::Find("AssetInspector.WorldLayers").has_value(); }));
		CHECK(UI::ItemProbe::Find("AssetInspector.WorldIndex").has_value());
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_AssetInspector)) == 0);
		// The world is open: Open has nothing to do.
		REQUIRE(UI::ItemProbe::Find("AssetInspector.OpenWorld"));
		CHECK_FALSE(UI::ItemProbe::Find("AssetInspector.OpenWorld")->Enabled);

		// Cook measures every cell into the world's index, off the frame.
		REQUIRE(editor.Click("AssetInspector.CookWorld"));
		REQUIRE(editor.FramesUntil([]()
		{
			const std::optional<UI::ItemProbe::Item> cook = UI::ItemProbe::Find("AssetInspector.WorldCook");
			return cook && cook->Text.rfind("Cooked 25 cells", 0) == 0;
		}));
		std::vector<uint8_t> bytes;
		REQUIRE(editor.Context().GetAssetManager()->ReadStoredData(world, bytes));
		const Ref<WorldAsset> cooked = WorldAsset::Deserialize(bytes);
		REQUIRE(cooked);
		REQUIRE(cooked->GetData().Index.has_value());
		CHECK(cooked->GetData().Index->Cells.size() == 25);

		// Away from the world, Open opens it again.
		editor.RunToCompletion("scene.new", { { "name", "Elsewhere" }, { "force", true } });
		REQUIRE_FALSE(editor.Context().GetWorld());
		editor.Frames(2);
		REQUIRE(editor.Click("AssetInspector.OpenWorld"));
		CHECK(editor.FramesUntil([&]() { return editor.Context().GetWorld() != nullptr; }));
	}

	TEST_CASE("New > World in the Content Browser makes a world in the open folder and opens it")
	{
		WorldEditor editor("WorldUINew");
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		editor.Layer->GetPanels().Focus(EditorPanels::c_ContentBrowser);
		browser->SetCurrentDirectory("Worlds");
		editor.Frames(3);
		REQUIRE(editor.Click("ContentBrowser.New"));
		editor.Frames(1);
		REQUIRE(editor.Click("ContentBrowser.New.World"));
		REQUIRE(editor.FramesUntil([&]()
		{
			const EditorWorld* world = editor.Context().GetWorld();
			return world && world->GetWorld() == editor.Context().GetAssetManager()->FindAssetByPath("Worlds/World.stworld");
		}));
		CHECK(editor.Context().GetAssetManager()->FindAssetByPath("Worlds/World.stscene") == editor.Context().GetSceneHandle());
		CHECK(editor.ProbeText("Hierarchy.WorldName") == "World");
	}
}
