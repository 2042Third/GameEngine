#include <doctest/doctest.h>

#include "Editor/CellBoundsOverlay.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SaveTestUtils.h"
#include "Editor/EditorWorld.h"
#include "Editor/SceneRaycast.h"
#include "Editor/WorldProjectUtils.h"
#include "EditorLayer.h"
#include "Panels/StreamingPanel.h"
#include "Physics/PhysicsTestUtils.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <cmath>
#include <optional>
#include <set>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor without a project, past the launcher, editing a scene of a floor and `boxes` boxes at rest on it.
	struct PhysicsEditor : HarnessEditor
	{
		explicit PhysicsEditor(uint32_t boxes, const ImGuiHarness::Specification& window = {})
			: HarnessEditor(window)
		{
			Layer->DismissLauncher();
			Scene& scene = *Context().GetActiveScene();
			CreateGround(scene);
			for (uint32_t index = 0; index < boxes; index++)
				CreateDynamicBox(scene, "Box " + std::to_string(index), glm::vec3(static_cast<float>(index) * 2.0f - 10.0f, 0.5f, 0.0f));
			Frames(2);
		}

		// The window that scrolls the Streaming panel's sections: the panel, or its overview beside the asset table.
		ImGuiWindow* FindStreamingSections()
		{
			ImGuiWindow* panel = FindPanelWindow(EditorPanels::c_Streaming);
			REQUIRE(panel);
			ImGuiWindow* sections = panel;
			for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
			{
				if (window->RootWindow == panel && std::string_view(window->Name).find("/Overview") != std::string_view::npos)
					sections = window;
			}
			return sections;
		}
	};

}

TEST_SUITE("Editor.World")
{
	TEST_CASE("physics.stats reports the running game's bodies, capacity and growth, and nothing while editing")
	{
		PhysicsEditor editor(10);
		CHECK(editor.Run("physics.stats") == nlohmann::json { { "running", false } });

		editor.Run("play.start");
		editor.Frames(3);
		nlohmann::json stats = editor.Run("physics.stats");
		CHECK(stats["running"] == true);
		CHECK(stats["simulating"] == true);
		CHECK(stats["bodies"]["total"] == 11);
		CHECK(stats["bodies"]["static"] == 1);
		CHECK(stats["bodies"]["dynamic"] == 10);
		CHECK(stats["bodies"]["slotsUsed"] == 11);
		CHECK(stats["capacity"]["maxBodies"] == 65536);
		CHECK(stats["capacity"]["reservedMB"].get<double>() > 1.0);
		CHECK(stats["waitingForSlot"] == 0);
		CHECK(stats["growths"]["count"] == 0);
		CHECK(stats["steps"].get<uint64_t>() > 0);

		// What the last frame built and destroyed.
		Scene& game = *editor.Context().GetActiveScene();
		CreateDynamicBox(game, "Spawned", glm::vec3(0.0f, 5.0f, 0.0f));
		editor.Frames(1);
		CHECK(editor.Run("physics.stats")["addedLastFrame"] == 1);
		editor.Frames(1);
		CHECK(editor.Run("physics.stats")["addedLastFrame"] == 0);

		editor.Run("play.stop");
		editor.Frames(1);
		CHECK(editor.Run("physics.stats") == nlohmann::json { { "running", false } });
	}

	TEST_CASE("The Physics section's facts wrap inside a narrow Streaming panel")
	{
		// An 800 x 900 window at 150%: the bottom dock is narrower than a line of the section's facts.
		PhysicsEditor editor(10, ImGuiHarness::Specification { ImVec2(800.0f, 900.0f), 1.5f });
		editor.Run("play.start");
		editor.Frames(3);
		editor.Run("editor.showPanel", { { "panel", "Streaming" } });
		editor.Frames(3);
		const ImGuiWindow* sections = editor.FindStreamingSections();
		for (const char* key : { "Streaming.PhysicsBodies", "Streaming.PhysicsPending", "Streaming.PhysicsReserved" })
		{
			CAPTURE(key);
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE(item);
			CHECK(item->Max.x <= sections->InnerClipRect.Max.x + 1.0f);
		}
		editor.Run("play.stop");
		editor.Frames(2);
	}

	TEST_CASE("Bodies without collision show in a pill that opens the Streaming panel's Physics section")
	{
		// A world held at its growth limit: 6 of the 11 bodies wait for a slot while the game runs.
		PhysicsSettings limited;
		limited.GrowthLimit = 5;
		ScopedPhysicsSettings scopedLimit(limited);
		PhysicsEditor editor(10);
		editor.Context().GetActiveScene()->GetSettings().PhysicsCapacity.MaxBodies = 5;
		CHECK_FALSE(UI::ItemProbe::Find("Status.Physics").has_value());

		editor.Run("play.start");
		editor.Frames(3);
		CHECK(editor.GetPillText("Status.Physics") == "6 bodies without collision");
		CHECK(editor.Run("physics.stats")["waitingForSlot"] == 6);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));

		REQUIRE(editor.Click("Status.Physics"));
		editor.Frames(3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		const StreamingPanel* streaming = editor.Layer->GetPanels().Get<StreamingPanel>(EditorPanels::c_Streaming);
		REQUIRE(streaming);
		REQUIRE(streaming->GetPhysics().has_value());
		CHECK(streaming->GetPhysics()->Bodies == 5);
		CHECK(streaming->GetPhysics()->MaxBodies == 5);
		const std::optional<UI::ItemProbe::Item> bodies = UI::ItemProbe::Find("Streaming.PhysicsBodies");
		REQUIRE(bodies);
		CHECK(UI::ItemProbe::Find("Streaming.PhysicsWaiting").has_value());

		// Scrolled away from the section, the pill brings it back to the top of the panel's sections.
		ImGuiWindow* sections = editor.FindStreamingSections();
		REQUIRE(sections->ScrollMax.y > 0.0f);
		ImGui::SetScrollY(sections, sections->ScrollMax.y);
		editor.Frames(2);
		auto headerInView = [&]()
		{
			const std::optional<UI::ItemProbe::Item> header = UI::ItemProbe::Find("Streaming.Physics");
			REQUIRE(header);
			return header->Min.y >= sections->InnerClipRect.Min.y - 1.0f && header->Max.y <= sections->InnerClipRect.Max.y + 1.0f;
		};
		CHECK_FALSE(headerInView());
		REQUIRE(editor.Click("Status.Physics"));
		editor.Frames(3);
		CHECK(headerInView());

		// Removed bodies make room: the waiting ones get theirs, and the pill goes.
		Scene& game = *editor.Context().GetActiveScene();
		std::vector<Entity> removed;
		for (uint32_t index = 0; index < 6; index++)
			removed.push_back(game.FindEntityByName("Box " + std::to_string(index)));
		game.DestroyEntities(removed);
		editor.Frames(3);
		CHECK(editor.Run("physics.stats")["waitingForSlot"] == 0);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Physics").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Streaming.PhysicsWaiting").has_value());

		// While editing there is no physics to show.
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK_FALSE(streaming->GetPhysics().has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Streaming.PhysicsBodies").has_value());
	}
}

namespace
{

	// The cells of a generated world's Ground layer within `radius` of a point, found independently of the streamer.
	std::set<std::pair<int32_t, int32_t>> GroundCellsWithin(uint32_t side, const glm::vec3& point, float radius)
	{
		std::set<std::pair<int32_t, int32_t>> cells;
		for (int32_t z = 0; z < static_cast<int32_t>(side); z++)
		{
			for (int32_t x = 0; x < static_cast<int32_t>(side); x++)
			{
				const double dx = std::max({ 0.0, x * c_GenCellSize - static_cast<double>(point.x), static_cast<double>(point.x) - (x + 1) * c_GenCellSize });
				const double dz = std::max({ 0.0, z * c_GenCellSize - static_cast<double>(point.z), static_cast<double>(point.z) - (z + 1) * c_GenCellSize });
				if (std::sqrt(dx * dx + dz * dz) <= radius)
					cells.insert({ x, z });
			}
		}
		return cells;
	}

	std::set<std::pair<int32_t, int32_t>> LoadedCells(const nlohmann::json& status)
	{
		std::set<std::pair<int32_t, int32_t>> cells;
		for (const nlohmann::json& cell : status["list"])
		{
			if (cell["state"] == "Loaded")
				cells.insert({ cell["x"].get<int32_t>(), cell["z"].get<int32_t>() });
		}
		return cells;
	}

	// Editor frames until the world settled: nothing loading or unloading.
	void Settle(GridWorldProject& project)
	{
		REQUIRE(WaitUntil([&]()
		{
			project.Context.Update(Timestep(1.0f / 60.0f));
			const nlohmann::json status = project.Run("world.status");
			return !status["world"].is_null() && status["cells"]["loading"] == 0 && status["cells"]["unloading"] == 0 && status.contains("name");
		}, std::chrono::seconds(60)));
		// One more frame, for an evaluation that a finished load started.
		project.Context.Update(Timestep(1.0f / 60.0f));
		REQUIRE(WaitUntil([&]()
		{
			project.Context.Update(Timestep(1.0f / 60.0f));
			const nlohmann::json status = project.Run("world.status");
			return status["cells"]["loading"] == 0 && status["cells"]["unloading"] == 0;
		}, std::chrono::seconds(60)));
	}

	// A step that changes the scene in a way only a snapshot captures (no entity states): it renames one entity.
	class RenameAction final : public EditorAction
	{
	public:
		RenameAction(Scene& scene, UUID entity, std::string name)
			: m_Scene(scene), m_Entity(entity), m_Name(std::move(name))
		{
		}

		const std::string& GetName() const override
		{
			static const std::string s_Name = "Rename";
			return s_Name;
		}

		bool Execute(std::string*) override
		{
			Entity entity = m_Scene.GetEntityByUUID(m_Entity);
			if (!entity)
				return false;
			m_Old = entity.GetName();
			entity.GetComponent<NameComponent>().Name = m_Name;
			entity.MarkModified<NameComponent>();
			return true;
		}

		bool Undo(std::string*) override
		{
			Entity entity = m_Scene.GetEntityByUUID(m_Entity);
			if (!entity)
				return false;
			entity.GetComponent<NameComponent>().Name = m_Old;
			entity.MarkModified<NameComponent>();
			return true;
		}
	private:
		Scene& m_Scene;
		UUID m_Entity;
		std::string m_Name;
		std::string m_Old;
	};

	void LookFrom(GridWorldProject& project, const glm::vec3& position)
	{
		project.Run("camera.set", { { "position", { position.x, position.y, position.z } }, { "target", { position.x, 0.0f, position.z - 10.0f } } });
	}

}

TEST_SUITE("Editor.World")
{
	TEST_CASE("Opening a world streams its cells around the editor camera, and moving the camera loads and unloads them")
	{
		GridWorldProject project("EditorWorldOpen", 5);
		CHECK(project.Run("world.status")["world"].is_null());
		CHECK(project.Run("scene.loadStatus")["world"].is_null());
		CHECK(project.Run("editor.status", { { "sections", { "world" } } })["world"].is_null());
		// The compact answer leaves the world out: it is asked for.
		CHECK_FALSE(project.Run("editor.status").contains("world"));
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		REQUIRE(project.Context.GetWorld());
		const glm::vec3 start(14.0f, 10.0f, 14.0f); // Off every cell's edge at 48 m
		LookFrom(project, start);
		Settle(project);
		nlohmann::json status = project.Run("world.status", { { "cells", true } });
		CHECK(status["world"] == UUIDToJson(project.World));
		CHECK(status["name"] == "Gen");
		CHECK(status["editLayer"] == "Persistent");
		CHECK(LoadedCells(status) == GroundCellsWithin(5, start, 48.0f));
		CHECK(status["cells"]["loaded"] == GroundCellsWithin(5, start, 48.0f).size());
		CHECK(status["cells"]["dirty"] == 0);
		CHECK(status["sources"][0]["position"][0].get<float>() == doctest::Approx(start.x));
		// scene.loadStatus and editor.status agree with it.
		const nlohmann::json loads = project.Run("scene.loadStatus")["world"];
		CHECK(loads["editor"]["cellsLoaded"] == status["cells"]["loaded"]);
		CHECK(loads["editor"]["cellsWanted"] == status["cells"]["wanted"]);
		CHECK(loads["editor"]["cellsLoading"] == 0);
		CHECK(loads["game"].is_null());
		const nlohmann::json summary = project.Run("editor.status", { { "sections", { "world" } } })["world"];
		CHECK(summary["name"] == "Gen");
		CHECK(summary["cellsLoaded"] == status["cells"]["loaded"]);
		CHECK(summary["cellsDirty"] == 0);
		// The cells' entities are in the edited scene, which is not modified by them.
		CHECK(project.Context.GetEditScene()->FindEntityByName("Box 0").IsValid());
		CHECK_FALSE(project.Context.IsSceneModified());

		// Far away, the first cells go and others come.
		const glm::vec3 far(140.0f, 10.0f, 140.0f);
		LookFrom(project, far);
		Settle(project);
		status = project.Run("world.status", { { "cells", true } });
		CHECK(LoadedCells(status) == GroundCellsWithin(5, far, 48.0f));

		// Hidden layers unload; the radius scale widens what loads.
		project.Run("world.edit", { { "radiusScale", 2.0 } });
		Settle(project);
		CHECK(LoadedCells(project.Run("world.status", { { "cells", true } })) == GroundCellsWithin(5, far, 96.0f));
		project.Run("world.edit", { { "visibleLayers", { "Ground" } } });
		CHECK_FALSE(project.Context.GetWorld()->GetVisibleLayers().empty());
		CHECK_FALSE(project.Try("world.edit", { { "visibleLayers", { "Rivers" } } }).Success);
		CHECK_FALSE(project.Try("world.edit", { { "editLayer", "Rivers" } }).Success);
		CHECK_FALSE(project.Try("world.edit", { { "radiusScale", 20.0 } }).Success);
		CHECK(project.Run("world.edit", { { "editLayer", "Ground" } })["editLayer"] == "Ground");
		CHECK(project.Context.GetWorld()->GetRadiusScale() == 2.0f);
	}

	TEST_CASE("The viewport's cell bounds outline the cells in reach, by state, and are worked out again only when cells change")
	{
		GridWorldProject project("EditorWorldCellBounds", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		const glm::vec3 start(14.0f, 10.0f, 14.0f);
		LookFrom(project, start);
		Settle(project);
		const EditorWorld& world = *project.Context.GetWorld();
		ViewportDebugColors colors;
		colors.CellLoaded = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
		CellBoundsOverlay overlay;
		DebugDraw lines;
		overlay.Draw(world.GetStreamer(), start, world.GetRadiusScale(), colors, lines);
		const size_t loaded = GroundCellsWithin(5, start, 48.0f).size();
		CHECK(overlay.GetDrawnCellCount() == loaded);
		// Four lines (eight vertices) per cell, over the scene, in the loaded color, at y = 0 (no cooked index).
		REQUIRE(lines.GetLines(DebugDrawDepth::OnTop).size() == 8 * loaded);
		CHECK(lines.GetLines(DebugDrawDepth::Tested).empty());
		CHECK(lines.GetLines(DebugDrawDepth::OnTop)[0].Color == 0xFF0000FFu);
		CHECK(lines.GetLines(DebugDrawDepth::OnTop)[0].Position.y == 0.0f);
		CHECK(overlay.GetBuildCount() == 1);

		// A still world draws them again without working them out; a camera far from them draws none.
		lines.Clear();
		overlay.Draw(world.GetStreamer(), start, world.GetRadiusScale(), colors, lines);
		CHECK(overlay.GetBuildCount() == 1);
		CHECK(lines.GetLines(DebugDrawDepth::OnTop).size() == 8 * loaded);
		lines.Clear();
		overlay.Draw(world.GetStreamer(), glm::vec3(1000.0f, 0.0f, 1000.0f), world.GetRadiusScale(), colors, lines);
		CHECK(overlay.GetDrawnCellCount() == 0);
		CHECK(lines.IsEmpty());

		// Cells that load and unload are outlined anew.
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		Settle(project);
		lines.Clear();
		overlay.Draw(world.GetStreamer(), glm::vec3(140.0f, 10.0f, 140.0f), world.GetRadiusScale(), colors, lines);
		CHECK(overlay.GetBuildCount() > 1);
		CHECK(overlay.GetDrawnCellCount() == GroundCellsWithin(5, glm::vec3(140.0f, 10.0f, 140.0f), 48.0f).size());
	}

	TEST_CASE("world.loadCells loads cells anywhere and keeps them until they are let go")
	{
		GridWorldProject project("EditorWorldLoadCells", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		const nlohmann::json loaded = project.Run("world.loadCells", { { "cells", { { { "layer", "Ground" }, { "x", 4 }, { "z", 4 } } } }, { "wait", true } });
		REQUIRE(loaded["cells"].size() == 1);
		CHECK(loaded["cells"][0]["state"] == "Loaded");
		CHECK(loaded["cells"][0]["entities"].get<uint64_t>() == 7 + 4);
		Settle(project);
		CHECK(LoadedCells(project.Run("world.status", { { "cells", true } })).contains({ 4, 4 }));

		// A region; then everything let go: only the camera's cells stay.
		const nlohmann::json region = project.Run("world.loadCells", { { "region", { { "min", { 100.0, 0.0 } }, { "max", { 159.0, 20.0 } } } }, { "wait", true } });
		CHECK(region["cells"].size() == 2); // (3, 0) and (4, 0)
		project.Run("world.loadCells", { { "unpinAll", true } });
		Settle(project);
		CHECK(LoadedCells(project.Run("world.status", { { "cells", true } })) == GroundCellsWithin(5, glm::vec3(14.0f, 10.0f, 14.0f), 48.0f));

		CHECK_FALSE(project.Try("world.loadCells", { { "cells", { { { "layer", "Ground" }, { "x", 9 }, { "z", 9 } } } } }).Success);
		CHECK_FALSE(project.Try("world.loadCells", { { "region", { { "center", { 0.0, 0.0 } } } } }).Success);
	}

	TEST_CASE("An edited cell is dirty, and stays loaded while the camera is away")
	{
		GridWorldProject project("EditorWorldDirty", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		// An entity of cell (0, 0).
		const Entity box = project.Context.GetEditScene()->FindEntityByName("Box 0");
		REQUIRE(box);
		project.Run("component.set", { { "entity", UUIDToJson(box.GetUUID()) }, { "component", "Transform" }, { "values", { { "Scale", { 2.0, 2.0, 2.0 } } } } });
		nlohmann::json status = project.Run("world.status", { { "cells", true } });
		CHECK(status["cells"]["dirty"] == 1);
		CHECK_FALSE(status["persistentDirty"].get<bool>());
		CHECK(project.Context.GetWorld()->IsCellDirty(project.GetCell(0, 0)));
		// A change of the persistent scene marks it.
		project.Run("entity.create", { { "name", "Lamp" } });
		CHECK(project.Run("world.status")["persistentDirty"] == true);

		// Ten radii away, the dirty cell stays while the others go.
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		Settle(project);
		status = project.Run("world.status", { { "cells", true } });
		const std::set<std::pair<int32_t, int32_t>> cells = LoadedCells(status);
		CHECK(cells.contains({ 0, 0 }));
		CHECK_FALSE(cells.contains({ 1, 0 }));
		CHECK(project.Context.GetEditScene()->GetEntityByUUID(box.GetUUID()).GetComponent<TransformComponent>().Scale.x == 2.0f);
	}

	TEST_CASE("A selected entity keeps its cell loaded, and frames that change nothing look the selection's cells up again")
	{
		GridWorldProject project("EditorWorldSelection", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		EditorWorld& world = *project.Context.GetWorld();
		const Entity box = project.Context.GetEditScene()->FindEntityByName("Box 0"); // Of cell (0, 0)
		REQUIRE(box);
		const uint64_t before = world.GetSelectionPinLookups();
		project.Context.Select(box.GetUUID());
		project.Context.Update(Timestep(1.0f / 60.0f));
		CHECK(world.GetSelectionPinLookups() == before + 1);
		// Idle frames: nothing the cells depend on changed.
		for (int frame = 0; frame < 10; frame++)
			project.Context.Update(Timestep(1.0f / 60.0f));
		CHECK(world.GetSelectionPinLookups() == before + 1);

		// Away from it, the selected entity's cell stays and its neighbours go.
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		Settle(project);
		std::set<std::pair<int32_t, int32_t>> cells = LoadedCells(project.Run("world.status", { { "cells", true } }));
		CHECK(cells.contains({ 0, 0 }));
		CHECK_FALSE(cells.contains({ 1, 0 }));
		CHECK_FALSE(project.Context.GetWorld()->IsCellDirty(project.GetCell(0, 0)));

		// Deselected, it goes too.
		project.Context.ClearSelection();
		Settle(project);
		cells = LoadedCells(project.Run("world.status", { { "cells", true } }));
		CHECK_FALSE(cells.contains({ 0, 0 }));
	}
}

namespace
{

	struct FileState
	{
		std::string Text;
		std::filesystem::file_time_type Time;

		bool operator==(const FileState&) const = default;
	};

	FileState ReadFileState(const std::filesystem::path& path)
	{
		FileState state;
		state.Text = FileSystem::ReadText(path).value_or(std::string());
		state.Time = std::filesystem::last_write_time(path);
		return state;
	}

}

TEST_SUITE("Editor.World")
{
	TEST_CASE("Saving a world writes only the dirty cell, and the persistent scene never holds the cells' entities")
	{
		GridWorldProject project("EditorWorldSave", 3);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		const std::filesystem::path assets = project.Directory / "Assets";
		const std::filesystem::path persistentFile = assets / "Scenes" / "GenPersistent.stscene";
		const std::filesystem::path cellA = assets / "Worlds" / "Gen" / "Ground" / "0_0.stscene";
		const std::filesystem::path cellB = assets / "Worlds" / "Gen" / "Ground" / "1_0.stscene";
		const std::filesystem::path worldFile = assets / "Worlds" / "Gen.stworld";
		const FileState persistentBefore = ReadFileState(persistentFile);
		const FileState cellBefore = ReadFileState(cellA);
		const FileState otherBefore = ReadFileState(cellB);
		// File times have a coarse grain on some systems: a write now must show.
		std::this_thread::sleep_for(std::chrono::milliseconds(20));

		// Cell A's box is edited, then saved.
		const Entity box = project.Context.GetEditScene()->FindEntityByName("Box 0");
		REQUIRE(box);
		const UUID boxID = box.GetUUID();
		REQUIRE(project.Context.GetEditScene()->GetSubSceneStatus(project.Context.GetEditScene()->GetSubSceneOf(box))->Scene == project.GetCell(0, 0));
		project.Run("component.set", { { "entity", UUIDToJson(box.GetUUID()) }, { "component", "Transform" }, { "values", { { "Scale", { 2.0, 2.0, 2.0 } } } } });
		const nlohmann::json saved = project.Run("scene.save");
		REQUIRE(saved["documents"].size() == 2); // The cell and the world's index
		CHECK(saved["documents"][0]["kind"] == "cell");
		CHECK(saved["documents"][0]["path"] == "Worlds/Gen/Ground/0_0.stscene");
		CHECK(saved["documents"][0]["entities"] == 7);
		CHECK(saved["documents"][1]["kind"] == "world");
		CHECK_FALSE(project.Context.IsSceneModified());
		CHECK(project.Run("world.status")["cells"]["dirty"] == 0);

		CHECK(ReadFileState(persistentFile) == persistentBefore);
		CHECK(ReadFileState(cellB) == otherBefore);
		CHECK_FALSE(ReadFileState(cellA) == cellBefore);
		// The cell's document holds its seven boxes, the scaled one among them, and no settings.
		const std::optional<nlohmann::json> cell = JsonUtils::Parse(ReadFileState(cellA).Text);
		REQUIRE(cell.has_value());
		CHECK((*cell)["Scene"]["Entities"].size() == 7);
		CHECK_FALSE((*cell)["Scene"].contains("Settings"));
		CHECK(ReadFileState(cellA).Text.find("\"Scale\": [\n\t\t\t\t\t\t\t2") != std::string::npos);
		// The world's index has the cell's entry, current.
		const Ref<WorldAsset> world = project.ReadWorld();
		REQUIRE(world->GetData().Index.has_value());
		const WorldCellSummary* entry = world->FindSummary(WorldCellKey { 1, 0, 0 });
		REQUIRE(entry);
		CHECK(entry->Entities == 7);
		CHECK(entry->Stamp == fmt::format("{:016X}", project.Context.GetAssetManager()->GetContentStamp(project.GetCell(0, 0))));
		(void)worldFile;

		// The persistent scene changes: its save leaves the cells' entities out.
		project.Run("entity.create", { { "name", "Lamp" } });
		const nlohmann::json second = project.Run("scene.save");
		REQUIRE(second["documents"].size() == 1);
		CHECK(second["documents"][0]["kind"] == "persistent");
		const std::string persistent = ReadFileState(persistentFile).Text;
		CHECK(persistent.find("\"Lamp\"") != std::string::npos);
		CHECK(persistent.find("\"Box 0\"") == std::string::npos);
		CHECK(persistent.find("\"Source\"") != std::string::npos);

		// Reopened, the scale is there.
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) }, { "force", true } });
		Settle(project);
		const Entity reopened = project.Context.GetEditScene()->GetEntityByUUID(boxID);
		REQUIRE(reopened);
		CHECK(reopened.GetComponent<TransformComponent>().Scale.x == 2.0f);
		CHECK_FALSE(project.Try("scene.saveAs", { { "path", "Scenes/Copy.stscene" } }).Success);
	}
}

namespace
{

	AssetHandle CellOf(EditorContext& context, UUID id)
	{
		Scene& scene = *context.GetEditScene();
		const Entity entity = scene.GetEntityByUUID(id);
		REQUIRE(entity);
		const SubSceneStatus* status = scene.GetSubSceneStatus(scene.GetSubSceneOf(entity));
		return status ? status->Scene : UUID::Null();
	}

}

TEST_SUITE("Editor.World")
{
	TEST_CASE("A move across a cell border is one undo step, and the save writes both cells")
	{
		GridWorldProject project("EditorWorldBorder", 3);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(30.0f, 10.0f, 14.0f));
		Settle(project);
		const Entity box = project.Context.GetEditScene()->FindEntityByName("Box 0");
		REQUIRE(box);
		const UUID id = box.GetUUID();
		const glm::vec3 before = box.GetComponent<TransformComponent>().Translation;
		REQUIRE(CellOf(project.Context, id) == project.GetCell(0, 0));

		// Into cell (1, 0).
		project.Run("component.set", { { "entity", UUIDToJson(id) }, { "component", "Transform" }, { "values", { { "Translation", { 40.0, 0.5, 4.0 } } } } });
		CHECK(CellOf(project.Context, id) == project.GetCell(1, 0));
		CHECK(project.Context.GetWorld()->IsCellDirty(project.GetCell(0, 0)));
		CHECK(project.Context.GetWorld()->IsCellDirty(project.GetCell(1, 0)));
		CHECK(project.Context.GetUndoStack().GetPosition() == 1);
		project.Run("edit.undo");
		CHECK(CellOf(project.Context, id) == project.GetCell(0, 0));
		CHECK(project.Context.GetEditScene()->GetEntityByUUID(id).GetComponent<TransformComponent>().Translation == before);
		project.Run("edit.redo");
		CHECK(CellOf(project.Context, id) == project.GetCell(1, 0));

		const nlohmann::json saved = project.Run("scene.save");
		std::set<std::string> paths;
		for (const nlohmann::json& document : saved["documents"])
			paths.insert(document["path"].get<std::string>());
		CHECK(paths.contains("Worlds/Gen/Ground/0_0.stscene"));
		CHECK(paths.contains("Worlds/Gen/Ground/1_0.stscene"));
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) }, { "force", true } });
		Settle(project);
		CHECK(CellOf(project.Context, id) == project.GetCell(1, 0));
	}

	TEST_CASE("New entities go into the edit layer's cell, new cells are written at the save, and entities move between layers")
	{
		GridWorldProject project("EditorWorldNewCells", 3);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);

		// The default edit layer is the persistent scene.
		const UUID lamp = GridWorldProject::ToUUID(project.Run("entity.create", { { "name", "Lamp" } })["id"]);
		CHECK(CellOf(project.Context, lamp) == UUID::Null());
		CHECK(project.Context.GetWorld()->GetDirtyCells().empty());

		// With the Ground as the edit layer, an entity placed where no cell exists makes one.
		project.Run("world.edit", { { "editLayer", "Ground" } });
		const UUID rock = GridWorldProject::ToUUID(project.Run("entity.create", { { "name", "Rock" }, { "components", { { "Transform", { { "Translation", { 112.0, 0.0, 16.0 } } } } } } })["id"]);
		const AssetHandle newCell = DeriveSubAssetHandle(project.World, "Cell/1/3/0");
		CHECK(CellOf(project.Context, rock) == newCell);
		const nlohmann::json saved = project.Run("scene.save");
		bool wroteCell = false;
		for (const nlohmann::json& document : saved["documents"])
			wroteCell = wroteCell || document["path"] == "Worlds/Gen/Ground/3_0.stscene";
		CHECK(wroteCell);
		CHECK(project.Context.GetAssetManager()->GetAssetType(newCell) == AssetType::Scene);
		CHECK(FileSystem::Exists(project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "3_0.stscene.meta"));
		const Ref<WorldAsset> world = project.ReadWorld();
		CHECK(world->FindCell(WorldCellKey { 1, 3, 0 }) >= 0);
		CHECK(world->GetData().Cells.size() == 10);

		// world.moveEntities: the rock to the persistent scene and back; a child is refused.
		project.Run("world.moveEntities", { { "entities", { UUIDToJson(rock) } }, { "layer", "Persistent" } });
		CHECK(CellOf(project.Context, rock) == UUID::Null());
		project.Run("world.moveEntities", { { "entities", { UUIDToJson(rock) } }, { "layer", "Ground" } });
		CHECK(CellOf(project.Context, rock) == newCell);
		const UUID pebble = GridWorldProject::ToUUID(project.Run("entity.create", { { "name", "Pebble" }, { "parent", UUIDToJson(rock) } })["id"]);
		CHECK_FALSE(project.Try("world.moveEntities", { { "entities", { UUIDToJson(pebble) } }, { "layer", "Persistent" } }).Success);
		CHECK_FALSE(project.Try("world.moveEntities", { { "entities", { UUIDToJson(rock) } }, { "layer", "Rivers" } }).Success);
		CHECK(CellOf(project.Context, rock) == newCell);
	}
}

TEST_SUITE("Editor.World")
{
	TEST_CASE("world.create makes a world and its persistent scene, opened or as one undo step")
	{
		GridWorldProject project("EditorWorldCreate", 2);
		const std::filesystem::path assets = project.Directory / "Assets";
		const nlohmann::json created = project.Run("world.create", { { "path", "Worlds/Island" }, { "cellSize", 32 } });
		CHECK(FileSystem::Exists(assets / "Worlds" / "Island.stworld"));
		CHECK(FileSystem::Exists(assets / "Worlds" / "Island.stscene"));
		REQUIRE(created["layers"].size() == 4);
		CHECK(created["layers"][0]["name"] == "Terrain");
		CHECK(created["layers"][0]["cellSize"] == 32.0);
		CHECK(created["layers"][0]["streamingDistance"] == 128.0);
		CHECK(created["layers"][0]["loadBeforeStart"] == true);
		CHECK(created["layers"][3]["loadBeforeStart"] == false);
		// Opened: the world is the edited one, with a camera that streams it.
		const UUID persistent = GridWorldProject::ToUUID(created["persistentScene"]);
		CHECK(project.Context.GetSceneHandle() == persistent);
		REQUIRE(project.Context.GetWorld());
		CHECK(project.Context.GetWorld()->GetWorld() == GridWorldProject::ToUUID(created["world"]));
		CHECK(project.Context.GetEditScene()->FindEntityByName("World").HasComponent<WorldComponent>());
		CHECK(project.Context.GetEditScene()->GetPrimaryCameraEntity().IsValid());

		// Without open: one undo step, which removes the files.
		const size_t steps = project.Context.GetUndoStack().GetPosition();
		project.Run("world.create", { { "path", "Worlds/Valley" }, { "layers", { "Ground", { { "name", "Trees" }, { "cellSize", 16 } } } }, { "open", false } });
		CHECK(FileSystem::Exists(assets / "Worlds" / "Valley.stworld"));
		CHECK(project.Context.GetUndoStack().GetPosition() == steps + 1);
		CHECK(project.Context.GetSceneHandle() == persistent);
		project.Run("edit.undo");
		CHECK_FALSE(FileSystem::Exists(assets / "Worlds" / "Valley.stworld"));
		CHECK_FALSE(FileSystem::Exists(assets / "Worlds" / "Valley.stscene"));

		// Invalid input writes nothing.
		CHECK_FALSE(project.Try("world.create", { { "path", "Worlds/Island" }, { "open", false } }).Success);
		CHECK_FALSE(project.Try("world.create", { { "path", "Worlds/Bad" }, { "cellSize", 0 }, { "open", false } }).Success);
		CHECK_FALSE(project.Try("world.create", { { "path", "Worlds/Bad" }, { "layers", { "A", "A" } }, { "open", false } }).Success);
		CHECK_FALSE(project.Try("world.create", { { "path", "../Outside" }, { "open", false } }).Success);
		CHECK_FALSE(FileSystem::Exists(assets / "Worlds" / "Bad.stworld"));
	}

	TEST_CASE("world.create waits for a save in flight before it opens the new world, which then needs no force")
	{
		GatedJobSystem jobs;
		GridWorldProject project("EditorWorldCreateAfterSave", 2);
		REQUIRE_FALSE(project.Context.GetWorld());
		project.Run("scene.saveAs", { { "path", "Scenes/Plain.stscene" } });
		project.Run("entity.create", { { "name", "Before The World" } });
		REQUIRE(project.Context.IsSceneModified());
		{
			BackgroundGate gate;
			REQUIRE(project.Commands.Execute(project.Context, "scene.save", { { "wait", false } }).Success);
			EditorCommandResult created = project.Commands.Execute(project.Context, "world.create", { { "path", "Worlds/Later" } });
			REQUIRE(created.IsPending());
			project.Context.Update(Timestep(1.0f / 60.0f));
			CHECK_FALSE(project.Context.GetWorld());
			gate.Open();
			created = CompleteCommand(project.Context, std::move(created));
			INFO(created.Error);
			REQUIRE(created.Success);
		}
		REQUIRE(project.Context.GetWorld());
		CHECK(project.Context.GetEditScene()->FindEntityByName("World").HasComponent<WorldComponent>());
	}
}

TEST_SUITE("Editor.World")
{
	TEST_CASE("world.setLayers adds, renames and removes layers in one undo step each")
	{
		GridWorldProject project("EditorWorldLayers", 2);
		const std::filesystem::path assets = project.Directory / "Assets";
		const std::string world = UUIDToJson(project.World).get<std::string>();

		// A layer added; the Ground keeps its ID.
		nlohmann::json result = project.Run("world.setLayers", { { "world", world }, { "layers", { "Ground", "Props" } } });
		REQUIRE(result["layers"].size() == 2);
		CHECK(result["layers"][0]["id"] == 1);
		CHECK(result["layers"][1]["name"] == "Props");
		CHECK(project.ReadWorld()->GetData().Cells.size() == 4);
		project.Run("edit.undo");
		CHECK(project.ReadWorld()->GetData().Layers.size() == 1);

		// A rename moves the layer's folder; the cells keep their handles.
		project.Run("world.setLayers", { { "world", world }, { "layers", { { { "id", 1 }, { "name", "Terrain" } } } } });
		CHECK(project.ReadWorld()->GetData().Layers[0].Name == "Terrain");
		CHECK(FileSystem::Exists(assets / "Worlds" / "Gen" / "Terrain" / "0_0.stscene"));
		CHECK_FALSE(FileSystem::Exists(assets / "Worlds" / "Gen" / "Ground"));
		CHECK(project.Context.GetAssetManager()->GetMetadata(project.GetCell(0, 0))->Path == "Worlds/Gen/Terrain/0_0.stscene");
		project.Run("edit.undo");
		CHECK(FileSystem::Exists(assets / "Worlds" / "Gen" / "Ground" / "0_0.stscene"));
		CHECK(project.ReadWorld()->GetData().Layers[0].Name == "Ground");

		// Removing the Ground, or re-gridding it, needs deleteCells; then its cells go to the trash until undone.
		CHECK_FALSE(project.Try("world.setLayers", { { "world", world }, { "layers", { "Props" } } }).Success);
		CHECK_FALSE(project.Try("world.setLayers", { { "world", world }, { "layers", { { { "name", "Ground" }, { "cellSize", 64 } } } } }).Success);
		CHECK_FALSE(project.Try("world.setLayers", { { "world", world }, { "layers", nlohmann::json::array() } }).Success);
		CHECK(project.ReadWorld()->GetData().Cells.size() == 4);
		result = project.Run("world.setLayers", { { "world", world }, { "layers", { "Props" } }, { "deleteCells", true } });
		CHECK(result["deletedCells"] == 4);
		CHECK(project.ReadWorld()->GetData().Cells.empty());
		CHECK_FALSE(FileSystem::Exists(assets / "Worlds" / "Gen" / "Ground" / "0_0.stscene"));
		project.Run("edit.undo");
		CHECK(project.ReadWorld()->GetData().Cells.size() == 4);
		CHECK(FileSystem::Exists(assets / "Worlds" / "Gen" / "Ground" / "0_0.stscene"));
		CHECK(project.Context.GetAssetManager()->GetAssetType(project.GetCell(0, 0)) == AssetType::Scene);
	}
}

TEST_SUITE("Editor.World")
{
	TEST_CASE("Playing a world copies the cells the editor has loaded, keeps the dirty ones and streams the rest")
	{
		GridWorldProject project("EditorWorldPlay", 8);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(46.0f, 10.0f, 46.0f));
		Settle(project);
		REQUIRE(project.Run("world.status")["cells"]["loaded"] == 9);
		// The first box of cell (0, 0), and those of cells (6, 6) and (1, 1) (GridWorldProject's IDs).
		const UUID boxID = UUID(0x6F0000000000ull);
		const UUID farBox = UUID(0x6F0000000000ull + (uint64_t(6 * 8 + 6) << 8));
		const UUID nearBox = UUID(0x6F0000000000ull + (uint64_t(1 * 8 + 1) << 8));
		REQUIRE(project.Context.GetEditScene()->GetEntityByUUID(boxID));
		project.Run("component.set", { { "entity", UUIDToJson(boxID) }, { "component", "Transform" }, { "values", { { "Scale", { 2.0, 2.0, 2.0 } } } } });
		const size_t editEntities = project.Context.GetEditScene()->GetEntityCount();

		project.Run("play.start");
		Scene& game = *project.Context.GetActiveScene();
		REQUIRE(&game != project.Context.GetEditScene().get());
		// The persistent scene and the nine cells, adopted as the game's sub-scenes: nothing loaded twice.
		CHECK(game.GetEntityCount() == editEntities);
		size_t adopted = 0;
		for (const SubSceneStatus& status : game.GetSubScenes())
			adopted += status.State == SubSceneState::Loaded ? 1 : 0;
		CHECK(adopted == 9);
		const IWorldStreaming* streaming = game.GetService<IWorldStreaming>();
		REQUIRE(streaming);
		CHECK(game.GetEntityByUUID(boxID).GetComponent<TransformComponent>().Scale.x == 2.0f);

		// The game's source moves far away: other cells come from their documents, the dirty cell stays.
		const Entity source = game.FindEntityByName("Source");
		REQUIRE(source);
		project.Run("component.set", { { "entity", UUIDToJson(source.GetUUID()) }, { "component", "Transform" }, { "values", { { "Translation", { 200.0, 0.0, 200.0 } } } } });
		REQUIRE(WaitUntil([&]()
		{
			project.Context.Update(Timestep(1.0f / 60.0f));
			const WorldStreamingStats stats = streaming->GetStats();
			return stats.CellsLoading == 0 && stats.CellsUnloading == 0 && game.GetEntityByUUID(farBox).IsValid();
		}, std::chrono::seconds(60)));
		CHECK(game.GetEntityByUUID(boxID));
		CHECK(game.GetEntityByUUID(boxID).GetComponent<TransformComponent>().Scale.x == 2.0f);
		CHECK_FALSE(game.GetEntityByUUID(nearBox).IsValid());
		CHECK(project.Run("world.status")["playing"]["loaded"].get<uint32_t>() >= 2);
		const nlohmann::json loads = project.Run("scene.loadStatus")["world"];
		CHECK(loads["game"]["cellsLoaded"] == streaming->GetStats().CellsLoaded);
		CHECK(loads["editor"]["cellsLoaded"] == project.Context.GetWorld()->GetStreamer().GetStats().CellsLoaded);

		project.Run("play.stop");
		CHECK(project.Context.GetEditScene()->GetEntityByUUID(boxID).GetComponent<TransformComponent>().Scale.x == 2.0f);
		CHECK(project.Context.GetEditScene()->GetEntityCount() == editEntities);
	}
}

TEST_SUITE("Editor.World")
{
	TEST_CASE("Undoing an edit of a cell that unloaded loads the cell and works once it is in")
	{
		GridWorldProject project("EditorWorldUndoUnloaded", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		const UUID boxID = UUID(0x6F0000000000ull); // Cell (0, 0)
		project.Run("component.set", { { "entity", UUIDToJson(boxID) }, { "component", "Transform" }, { "values", { { "Scale", { 2.0, 2.0, 2.0 } } } } });
		// Unsaved, the world refuses to close.
		CHECK_FALSE(project.Try("project.close", nlohmann::json::object()).Success);
		project.Run("scene.save");

		// Saved, the cell is no longer kept: it unloads when the camera leaves.
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		Settle(project);
		REQUIRE_FALSE(project.Context.GetEditScene()->GetEntityByUUID(boxID));
		const EditorCommandResult refused = project.Try("edit.undo", nlohmann::json::object());
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("the step changes cell Ground (0, 0), which is not loaded; it is loading now") != std::string::npos);
		Settle(project);
		REQUIRE(project.Context.GetEditScene()->GetEntityByUUID(boxID));
		project.Run("edit.undo");
		CHECK(project.Context.GetEditScene()->GetEntityByUUID(boxID).GetComponent<TransformComponent>().Scale.x == 0.5f);
		CHECK(project.Context.GetWorld()->IsCellDirty(project.GetCell(0, 0)));
	}

	TEST_CASE("A world's discarded edits are recovered into their cells, which load first")
	{
		GridWorldProject project("EditorWorldRecovery", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		// Cell (0, 0): one box scaled, one moved across the border into cell (1, 0). Cell (1, 1): a box deleted, nothing else.
		// The persistent scene: its settings only.
		const UUID scaled = UUID(0x6F0000000000ull);
		const UUID moved = UUID(0x6F0000000003ull);
		const UUID deleted = UUID(0x6F0000000000ull + (static_cast<uint64_t>(1 * 5 + 1) << 8) + 2);
		project.Run("component.set", { { "entity", UUIDToJson(scaled) }, { "component", "Transform" }, { "values", { { "Scale", { 2.0, 2.0, 2.0 } } } } });
		project.Run("component.set", { { "entity", UUIDToJson(moved) }, { "component", "Transform" }, { "values", { { "Translation", { 40.0, 3.5, 3.0 } } } } });
		project.Run("entity.delete", { { "entities", { UUIDToJson(deleted) } } });
		project.Run("scene.setSettings", { { "gravity", { 0.0, -5.0, 0.0 } } });

		// Away from them, the dirty cells stay; closing without force is refused, with force the changes become recovery data.
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		Settle(project);
		CHECK(project.Context.GetEditScene()->GetEntityByUUID(scaled));
		CHECK_FALSE(project.Try("project.close", nlohmann::json::object()).Success);
		const std::filesystem::path projectFile = project.Context.GetProject()->GetProjectFile();
		CHECK(project.Run("project.close", { { "force", true } })["discardedChanges"] == true);

		project.Run("project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		const nlohmann::json entries = project.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["applicable"] == true);
		// The journal names the cells, also of the deleted box.
		const std::string journal = FileSystem::ReadText(project.Directory / ".strata" / "Recovery" / (entries[0]["id"].get<std::string>() + ".strecovery")).value_or("");
		CHECK(journal.find(fmt::format("\"Cell\":\"{}\"", project.GetCell(0, 0).ToString())) != std::string::npos);
		CHECK(journal.find(fmt::format("\"Cell\":\"{}\"", project.GetCell(1, 0).ToString())) != std::string::npos);
		CHECK(journal.find(fmt::format("\"Cell\":\"{}\"", project.GetCell(1, 1).ToString())) != std::string::npos);

		// The editor camera is far from both cells: the recovery loads them before it applies.
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		project.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		Scene& scene = *project.Context.GetEditScene();
		auto cellOf = [&scene](UUID id)
		{
			const Entity entity = scene.GetEntityByUUID(id);
			const SubSceneStatus* status = entity ? scene.GetSubSceneStatus(scene.GetSubSceneOf(entity)) : nullptr;
			return status ? status->Scene : UUID::Null();
		};
		REQUIRE(scene.GetEntityByUUID(scaled));
		CHECK(scene.GetEntityByUUID(scaled).GetComponent<TransformComponent>().Scale.x == 2.0f);
		CHECK(cellOf(scaled) == project.GetCell(0, 0));
		CHECK(cellOf(moved) == project.GetCell(1, 0));
		CHECK(scene.IsSubSceneLoaded(scene.FindSubSceneBySource(project.GetCell(1, 1))));
		CHECK_FALSE(scene.GetEntityByUUID(deleted));
		CHECK(scene.GetSettings().Gravity.y == -5.0f);
		// One copy of each entity of the cells: what the cell documents hold met the recovered states.
		std::set<UUID> boxes;
		size_t boxCount = 0;
		for (const auto [handle, name] : scene.GetRegistry().view<const NameComponent>().each())
		{
			if (name.Name.starts_with("Box "))
			{
				boxes.insert(scene.GetRegistry().get<IDComponent>(handle).ID);
				boxCount++;
			}
		}
		CHECK(boxCount == boxes.size());
		CHECK(boxes.contains(moved));

		// The save writes the three cells, the persistent scene (its settings) and the world's index of the cells.
		const nlohmann::json saved = project.Run("scene.save");
		std::set<std::string> paths;
		for (const nlohmann::json& document : saved["documents"])
			paths.insert(document["path"].get<std::string>());
		CHECK(paths == std::set<std::string> { "Scenes/GenPersistent.stscene", "Worlds/Gen.stworld", "Worlds/Gen/Ground/0_0.stscene", "Worlds/Gen/Ground/1_0.stscene",
			"Worlds/Gen/Ground/1_1.stscene" });
		const std::optional<nlohmann::json> persistent = JsonUtils::Parse(FileSystem::ReadText(project.Directory / "Assets" / "Scenes" / "GenPersistent.stscene").value_or(""));
		REQUIRE(persistent.has_value());
		CHECK((*persistent)["Scene"]["Settings"]["Gravity"][1] == -5.0);
		CHECK(FileSystem::ReadText(project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "1_1.stscene").value_or("").find(deleted.ToString()) == std::string::npos);
		const std::string cell00 = FileSystem::ReadText(project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "0_0.stscene").value_or("");
		CHECK(cell00.find(moved.ToString()) == std::string::npos);
		const std::string cell10 = FileSystem::ReadText(project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "1_0.stscene").value_or("");
		CHECK(cell10.find(moved.ToString()) != std::string::npos);
	}

	TEST_CASE("Recovering a world makes its unsaved new cells again, and a snapshot leaves the cells it did not hold")
	{
		GridWorldProject project("EditorWorldRecoverySnapshot", 5);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);
		// A tower in a Ground cell no document holds yet, then a step only a snapshot captures (a rename of a box of cell
		// (0, 0)): the journal becomes a snapshot of what is loaded.
		project.Run("world.edit", { { "editLayer", "Ground" } });
		const UUID tower = GridWorldProject::ToUUID(project.Run("entity.create", { { "name", "Tower" },
			{ "components", { { "Transform", { { "Translation", { 200.0, 0.0, 10.0 } } } } } } })["id"]);
		const AssetHandle towerCell = DeriveSubAssetHandle(project.World, "Cell/1/6/0");
		REQUIRE(project.Context.GetWorld()->IsCellDirty(towerCell));
		const UUID renamed = UUID(0x6F0000000000ull);
		REQUIRE(project.Context.GetUndoStack().Execute(CreateScope<RenameAction>(*project.Context.GetEditScene(), renamed, "Renamed")));
		const std::filesystem::path projectFile = project.Context.GetProject()->GetProjectFile();
		CHECK(project.Run("project.close", { { "force", true } })["discardedChanges"] == true);

		project.Run("project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		const nlohmann::json entries = project.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["complete"] == true);
		// Recovered far away: cells (3, 3) to (4, 4) are loaded around the camera, which the snapshot never held.
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(140.0f, 10.0f, 140.0f));
		Settle(project);
		Scene& scene = *project.Context.GetEditScene();
		const SubSceneID far = scene.FindSubSceneBySource(project.GetCell(4, 4));
		REQUIRE(scene.IsSubSceneLoaded(far));
		project.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		REQUIRE(&scene == project.Context.GetEditScene().get());
		const Entity recoveredTower = scene.GetEntityByUUID(tower);
		REQUIRE(recoveredTower);
		const SubSceneStatus* status = scene.GetSubSceneStatus(scene.GetSubSceneOf(recoveredTower));
		REQUIRE(status);
		CHECK(status->Scene == towerCell);
		CHECK(project.Context.GetWorld()->IsCellDirty(towerCell));
		REQUIRE(scene.GetEntityByUUID(renamed));
		CHECK(scene.GetEntityByUUID(renamed).GetName() == "Renamed");
		CHECK(scene.GetSubSceneRoots(far).size() == 7 + 4);

		// The save writes the new cell with its .meta and lists it in the world.
		const nlohmann::json saved = project.Run("scene.save");
		bool wroteTower = false;
		for (const nlohmann::json& document : saved["documents"])
			wroteTower |= document["path"] == "Worlds/Gen/Ground/6_0.stscene";
		CHECK(wroteTower);
		CHECK(FileSystem::Exists(project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "6_0.stscene.meta"));
		CHECK(project.ReadWorld()->FindCell(WorldCellKey { 1, 6, 0 }) >= 0);
	}
}

TEST_SUITE("World.Coordinates")
{
	TEST_CASE("The editor's rays hit the entities of a world 20 km out")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		RunCommandToCompletion(context, commands, "project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("WorldCoordinatesRay") / "Game") }, { "name", "Game" } });
		// A floor of one 32 m cell (its top at y = 0) and a unit cube resting on it, around (20,016, 0, 16).
		auto create = [&](const char* name, const glm::vec3& position, const glm::vec3& scale)
		{
			const nlohmann::json created = RunCommandToCompletion(context, commands, "entity.create", { { "name", name }, { "components", {
				{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } },
				{ "Transform", { { "Translation", { position.x, position.y, position.z } }, { "Scale", { scale.x, scale.y, scale.z } } } } } } });
			return *UUIDFromJson(created["id"]);
		};
		const glm::vec3 there(20'016.0f, 0.0f, 16.0f);
		const UUID floor = create("Far Floor", there + glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(32.0f, 1.0f, 32.0f));
		const UUID cube = create("Far Cube", there + glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(1.0f));
		Scene& scene = *context.GetActiveScene();

		std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, Ray(there + glm::vec3(0.2f, 10.0f, 0.2f), glm::vec3(0.0f, -1.0f, 0.0f)));
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetUUID() == cube);
		CHECK(std::abs(hit->Point.y - 1.0f) < 0.005f);
		hit = SceneRaycast::Cast(scene, Ray(there + glm::vec3(10.0f, 10.0f, 10.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetUUID() == floor);
		CHECK(std::abs(hit->Point.y) < 0.005f);
		// From the side, 20 km out: the cube's face.
		hit = SceneRaycast::Cast(scene, Ray(there + glm::vec3(-8.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
		REQUIRE(hit);
		CHECK(hit->HitEntity.GetUUID() == cube);
		CHECK(std::abs(hit->Point.x - (there.x - 0.5f)) < 0.005f);
	}
}
