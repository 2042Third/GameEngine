#include <doctest/doctest.h>

#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/EditorWorld.h"
#include "Editor/WorldProjectUtils.h"
#include "EditorLayer.h"
#include "Panels/StreamingPanel.h"
#include "Physics/PhysicsTestUtils.h"
#include "UI/ItemProbe.h"

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

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor without a project, past the launcher, editing a scene of a floor and `boxes` boxes at rest on it.
	struct PhysicsEditor : HarnessEditor
	{
		explicit PhysicsEditor(uint32_t boxes)
		{
			Layer->DismissLauncher();
			Scene& scene = *Context().GetActiveScene();
			CreateGround(scene);
			for (uint32_t index = 0; index < boxes; index++)
				CreateDynamicBox(scene, "Box " + std::to_string(index), glm::vec3(static_cast<float>(index) * 2.0f - 10.0f, 0.5f, 0.0f));
			Frames(2);
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
	void Settle(WorldProject& project)
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

	void LookFrom(WorldProject& project, const glm::vec3& position)
	{
		project.Run("camera.set", { { "position", { position.x, position.y, position.z } }, { "target", { position.x, 0.0f, position.z - 10.0f } } });
	}

}

TEST_SUITE("Editor.World")
{
	TEST_CASE("Opening a world streams its cells around the editor camera, and moving the camera loads and unloads them")
	{
		WorldProject project("EditorWorldOpen", 5);
		CHECK(project.Run("world.status")["world"].is_null());
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

	TEST_CASE("world.loadCells loads cells anywhere and keeps them until they are let go")
	{
		WorldProject project("EditorWorldLoadCells", 5);
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
		WorldProject project("EditorWorldDirty", 5);
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
		WorldProject project("EditorWorldSave", 3);
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
		WorldProject project("EditorWorldBorder", 3);
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
		WorldProject project("EditorWorldNewCells", 3);
		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		LookFrom(project, glm::vec3(14.0f, 10.0f, 14.0f));
		Settle(project);

		// The default edit layer is the persistent scene.
		const UUID lamp = WorldProject::ToUUID(project.Run("entity.create", { { "name", "Lamp" } })["id"]);
		CHECK(CellOf(project.Context, lamp) == UUID::Null());
		CHECK(project.Context.GetWorld()->GetDirtyCells().empty());

		// With the Ground as the edit layer, an entity placed where no cell exists makes one.
		project.Run("world.edit", { { "editLayer", "Ground" } });
		const UUID rock = WorldProject::ToUUID(project.Run("entity.create", { { "name", "Rock" }, { "components", { { "Transform", { { "Translation", { 112.0, 0.0, 16.0 } } } } } } })["id"]);
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
		const UUID pebble = WorldProject::ToUUID(project.Run("entity.create", { { "name", "Pebble" }, { "parent", UUIDToJson(rock) } })["id"]);
		CHECK_FALSE(project.Try("world.moveEntities", { { "entities", { UUIDToJson(pebble) } }, { "layer", "Persistent" } }).Success);
		CHECK_FALSE(project.Try("world.moveEntities", { { "entities", { UUIDToJson(rock) } }, { "layer", "Rivers" } }).Success);
		CHECK(CellOf(project.Context, rock) == newCell);
	}
}
