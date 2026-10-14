#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/WorldCook.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scene/SceneServices.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldComponents.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_WorldHandle = 0x6E0000000001;
	constexpr uint64_t c_PersistentHandle = 0x6E0000000002;
	constexpr float c_CellSize = 32.0f;
	constexpr float c_SourceRadius = 40.0f;

	std::string ToText(const nlohmann::json& json)
	{
		return JsonUtils::Dump(json, 1, '\t') + "\n";
	}

	void WriteAsset(const std::filesystem::path& assets, const std::string& path, const nlohmann::json& document, uint64_t handle, const char* type)
	{
		const std::filesystem::path file = assets / FileSystem::FromUTF8(path);
		REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
		REQUIRE(FileSystem::WriteText(file, ToText(document)));
		const nlohmann::json meta = { { "Handle", UUID(handle).ToString() }, { "ImportSettings", nlohmann::json::object() },
			{ "Strata", { { "Format", "AssetMeta" }, { "Version", 1 } } }, { "Type", type } };
		REQUIRE(FileSystem::WriteText(FileSystem::FromUTF8(FileSystem::ToUTF8(file) + ".meta"), ToText(meta)));
	}

	// A new project with a generated world "Gen": `side` x `side` Ground cells of 32 m, cell (x, z) holding
	// 7 + x renderers of which (3 x + 5 z) % 7 are static boxes (uneven, so that the hysteresis changes the busiest radius),
	// and a persistent scene with the WorldComponent, a source of radius 40 m and a floor (one body).
	struct WorldProject
	{
		std::filesystem::path Directory;
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		AssetHandle World = UUID(c_WorldHandle);
		AssetHandle Persistent = UUID(c_PersistentHandle);
		std::map<std::pair<int32_t, int32_t>, Scene*> CellScenes;
		std::vector<Scope<Scene>> Cells;
		uint32_t Side = 0;

		WorldProject(const std::string& name, uint32_t side)
			: Side(side)
		{
			// A new project (its example script used by no scene), closed while the world's files are written into it.
			Run("project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory(name) / "Project") }, { "name", "Worlds" } });
			const std::filesystem::path projectFile = Context.GetProject()->GetProjectFile();
			Directory = projectFile.parent_path();
			Run("project.close", { { "force", true } });
			const std::filesystem::path assets = Directory / "Assets";
			WorldData data;
			data.Name = "Gen";
			data.PersistentScene = Persistent;
			data.Layers.push_back(WorldLayer { 1, "Ground", c_CellSize, 48.0f, true, true });
			for (uint32_t z = 0; z < side; z++)
			{
				for (uint32_t x = 0; x < side; x++)
				{
					const AssetHandle handle = DeriveSubAssetHandle(World, fmt::format("Cell/1/{}/{}", x, z));
					Scope<Scene> cell = CreateScope<Scene>(fmt::format("Gen {}_{}", x, z));
					const uint32_t entities = 7 + x;
					const uint32_t bodies = (3 * x + 5 * z) % 7;
					for (uint32_t index = 0; index < entities; index++)
					{
						Entity entity = cell->CreateEntityWithUUID(UUID(0x6F0000000000ull + (static_cast<uint64_t>(z * side + x) << 8) + index), fmt::format("Box {}", index));
						TransformComponent& transform = entity.GetComponent<TransformComponent>();
						transform.Translation = glm::vec3(x * c_CellSize + 4.0f + 7.0f * static_cast<float>(index % 4), 0.5f + static_cast<float>(index),
							z * c_CellSize + 3.0f + 5.0f * static_cast<float>(index / 4));
						transform.Scale = glm::vec3(0.5f);
						entity.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
						if (index < bodies)
							entity.AddComponent<BoxColliderComponent>();
					}
					WriteAsset(assets, fmt::format("Worlds/Gen/Ground/{}_{}.stscene", x, z), SceneSerializer::Serialize(*cell), static_cast<uint64_t>(handle), "Scene");
					data.Cells.push_back(WorldCell { 1, static_cast<int32_t>(x), static_cast<int32_t>(z), handle });
					CellScenes[{ x, z }] = cell.get();
					Cells.push_back(std::move(cell));
				}
			}
			Scene persistent("GenPersistent");
			persistent.CreateEntityWithUUID(UUID(0x6E0000000100ull), "World").AddComponent<WorldComponent>().World = World;
			Entity source = persistent.CreateEntityWithUUID(UUID(0x6E0000000101ull), "Source");
			source.AddComponent<StreamingSourceComponent>().Radius = c_SourceRadius;
			persistent.CreateEntityWithUUID(UUID(0x6E0000000102ull), "Floor").AddComponent<BoxColliderComponent>();
			WriteAsset(assets, "Scenes/GenPersistent.stscene", SceneSerializer::Serialize(persistent), c_PersistentHandle, "Scene");
			WriteAsset(assets, "Worlds/Gen.stworld", WorldAsset::ToDocument(data), c_WorldHandle, "World");

			Run("project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
			Context.GetAssetManager()->WaitForImports();
			REQUIRE(Context.GetAssetManager()->GetAssetType(World) == AssetType::World);
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Try(std::string_view name, const nlohmann::json& parameters)
		{
			return CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}

		AssetHandle GetCell(int32_t x, int32_t z) const
		{
			return DeriveSubAssetHandle(World, fmt::format("Cell/1/{}/{}", x, z));
		}

		Ref<WorldAsset> ReadWorld()
		{
			std::vector<uint8_t> bytes;
			REQUIRE(Context.GetAssetManager()->ReadStoredData(World, bytes));
			Ref<WorldAsset> world = WorldAsset::Deserialize(bytes);
			REQUIRE(world);
			return world;
		}
	};

	double SquareDistance(int32_t ax, int32_t az, int32_t bx, int32_t bz)
	{
		const double dx = std::max(0.0, std::abs(static_cast<double>(ax - bx)) - 1.0) * c_CellSize;
		const double dz = std::max(0.0, std::abs(static_cast<double>(az - bz)) - 1.0) * c_CellSize;
		return std::sqrt(dx * dx + dz * dz);
	}

}

TEST_SUITE("Editor.WorldCook")
{
	TEST_CASE("The cook's index equals an independent count, and its MaxBodiesInRadius a brute-force evaluation")
	{
		WorldProject project("WorldCookCounts", 5);
		const nlohmann::json result = project.Run("world.cook", { { "world", UUIDToJson(project.World) } });
		CHECK(result["cells"] == 25);
		CHECK(result["cellsRead"] == 25);
		CHECK(result["cellsReused"] == 0);
		CHECK(result["persistentBodies"] == 1);
		CHECK(result["sources"] == 1);
		CHECK(result["staleBefore"] == 25);
		CHECK(result["applied"] == false);
		REQUIRE(result["warnings"].size() == 1); // The persistent scene is not open: no capacity set
		CHECK(result["warnings"][0].get<std::string>().find("not the edited scene") != std::string::npos);
		CHECK(result["seconds"].get<double>() >= 0.0);

		const Ref<WorldAsset> world = project.ReadWorld();
		const WorldData& data = world->GetData();
		REQUIRE(data.Index.has_value());
		REQUIRE(data.Index->Cells.size() == 25);
		const std::optional<AABB> cube = project.Context.GetAssetManager()->GetBounds(BuiltinAssets::CubeMesh);
		std::map<std::pair<int32_t, int32_t>, uint64_t> bodies;
		for (const WorldCellSummary& summary : data.Index->Cells)
		{
			CAPTURE(summary.X);
			CAPTURE(summary.Z);
			const uint32_t x = static_cast<uint32_t>(summary.X);
			const uint32_t z = static_cast<uint32_t>(summary.Z);
			CHECK(summary.Entities == 7 + x);
			CHECK(summary.Bodies == (3 * x + 5 * z) % 7);
			bodies[{ summary.X, summary.Z }] = summary.Bodies;
			// Bounds: every box's position, grown by half its size where a collider or recorded mesh bounds cover it.
			glm::vec3 minimum(std::numeric_limits<float>::max());
			glm::vec3 maximum(std::numeric_limits<float>::lowest());
			const Scene& cell = *project.CellScenes.at({ summary.X, summary.Z });
			for (const entt::entity handle : cell.GetRegistry().view<const TransformComponent>())
			{
				const glm::vec3 position = cell.GetRegistry().get<TransformComponent>(handle).Translation;
				const bool extended = cube.has_value() || cell.GetRegistry().all_of<BoxColliderComponent>(handle);
				minimum = glm::min(minimum, position - glm::vec3(extended ? 0.25f : 0.0f));
				maximum = glm::max(maximum, position + glm::vec3(extended ? 0.25f : 0.0f));
			}
			for (int axis = 0; axis < 3; axis++)
			{
				CHECK(summary.BoundsMin[axis] == doctest::Approx(minimum[axis]));
				CHECK(summary.BoundsMax[axis] == doctest::Approx(maximum[axis]));
			}
			// The cells use built-in assets only: what loading one reads is its own file.
			const std::filesystem::path file = project.Directory / "Assets" / FileSystem::FromUTF8(fmt::format("Worlds/Gen/Ground/{}_{}.stscene", x, z));
			CHECK(summary.ClosureBytes == std::filesystem::file_size(file));
			CHECK(summary.Stamp == fmt::format("{:016X}", project.Context.GetAssetManager()->GetContentStamp(project.GetCell(summary.X, summary.Z))));
		}

		// Brute force: per cell, the bodies of every cell within the source's radius plus the hysteresis of any point of it.
		const double reach = c_SourceRadius + std::max(c_CellSize * 0.5, 0.1 * c_SourceRadius);
		uint64_t most = 0;
		for (const auto& [cell, unused] : bodies)
		{
			uint64_t sum = 0;
			for (const auto& [other, count] : bodies)
				sum += SquareDistance(cell.first, cell.second, other.first, other.second) <= reach ? count : 0;
			most = std::max(most, sum);
		}
		CHECK(data.Index->MaxBodiesInRadius == most + 1);
		CHECK(data.Index->PersistentBodies == 1);
		CHECK(result["maxBodiesInRadius"] == most + 1);
		CHECK(result["recommendedMaxBodies"] == (5 * (most + 1) + 3) / 4);
		CHECK(result["maxBodies"] == result["recommendedMaxBodies"]);
		CHECK(result["reservedMB"].get<double>() > 0.0);

		const nlohmann::json info = project.Run("world.info", { { "world", "Worlds/Gen.stworld" } });
		CHECK(info["cells"] == 25);
		CHECK(info["layers"][0]["name"] == "Ground");
		CHECK(info["layers"][0]["cells"] == 25);
		CHECK(info["index"]["cooked"] == true);
		CHECK(info["index"]["staleCells"] == 0);
		CHECK(info["index"]["maxBodiesInRadius"] == most + 1);
	}

	TEST_CASE("A second cook reads only the cells whose files changed")
	{
		WorldProject project("WorldCookStamps", 3);
		CHECK(project.Run("world.info", { { "world", UUIDToJson(project.World) } })["index"]["cooked"] == false);
		CHECK(project.Run("world.cook", { { "world", UUIDToJson(project.World) } })["cellsRead"] == 9);
		const nlohmann::json again = project.Run("world.cook", { { "world", UUIDToJson(project.World) } });
		CHECK(again["cellsRead"] == 0);
		CHECK(again["cellsReused"] == 9);
		CHECK(project.Context.GetUndoStack().GetPosition() == 1); // The second cook changed nothing

		// One cell gains an entity: only it is read again, and the index follows.
		Scene& cell = *project.CellScenes.at({ 1, 1 });
		cell.CreateEntityWithUUID(UUID(0x6F00000F0000ull), "Late").AddComponent<BoxColliderComponent>();
		const std::filesystem::path file = project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "1_1.stscene";
		REQUIRE(FileSystem::WriteText(file, ToText(SceneSerializer::Serialize(cell))));
		const AssetHandle handle = project.GetCell(1, 1);
		const uint64_t stamp = project.Context.GetAssetManager()->GetContentStamp(handle);
		project.Context.GetAssetManager()->ReimportAsset(handle);
		REQUIRE(WaitUntil([&]()
		{
			project.Context.Update(Timestep(1.0f / 60.0f));
			return project.Context.GetAssetManager()->GetContentStamp(handle) != stamp;
		}, std::chrono::seconds(30)));
		CHECK(project.Run("world.info", { { "world", UUIDToJson(project.World) } })["index"]["staleCells"] == 1);
		const nlohmann::json third = project.Run("world.cook", { { "world", UUIDToJson(project.World) } });
		CHECK(third["cellsRead"] == 1);
		CHECK(third["cellsReused"] == 8);
		CHECK(project.ReadWorld()->FindSummary(WorldCellKey { 1, 1, 1 })->Entities == 8 + 1);
	}

	TEST_CASE("apply sets the persistent scene's capacity in one undo step; invalid input changes nothing")
	{
		WorldProject project("WorldCookApply", 3);
		// The persistent scene must be the edited one.
		const EditorCommandResult refused = project.Try("world.cook", { { "world", UUIDToJson(project.World) }, { "apply", true } });
		CHECK_FALSE(refused.Success);
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK_FALSE(project.ReadWorld()->GetData().Index.has_value());
		CHECK_FALSE(project.Try("world.cook", { { "world", "Scenes/GenPersistent.stscene" } }).Success);
		CHECK_FALSE(project.Try("world.cook", {}).Success);
		CHECK(project.Context.GetUndoStack().GetPosition() == 0);

		project.Run("scene.open", { { "scene", UUIDToJson(project.Persistent) } });
		const size_t undos = project.Context.GetUndoStack().GetPosition();
		const nlohmann::json result = project.Run("world.cook", { { "world", UUIDToJson(project.World) }, { "apply", true } });
		CHECK(result["applied"] == true);
		const uint32_t recommended = result["recommendedMaxBodies"].get<uint32_t>();
		CHECK(recommended > 0);
		CHECK(project.Context.GetEditScene()->GetSettings().PhysicsCapacity.MaxBodies == recommended);
		CHECK(project.Context.GetUndoStack().GetPosition() == undos + 1);
		CHECK(project.ReadWorld()->GetData().Index.has_value());

		project.Run("edit.undo");
		CHECK(project.Context.GetEditScene()->GetSettings().PhysicsCapacity.MaxBodies == 0);
		CHECK_FALSE(project.ReadWorld()->GetData().Index.has_value());
		project.Run("edit.redo");
		CHECK(project.Context.GetEditScene()->GetSettings().PhysicsCapacity.MaxBodies == recommended);
		CHECK(project.ReadWorld()->GetData().Index.has_value());
	}

	TEST_CASE("Entity IDs shared by cells fail the cook, naming them and the cells")
	{
		WorldProject project("WorldCookDuplicates", 2);
		// Cell (1, 0) gets an entity with the ID of one of cell (0, 1)'s.
		Scene& cell = *project.CellScenes.at({ 1, 0 });
		const UUID shared = project.CellScenes.at({ 0, 1 })->GetEntitiesInHierarchyOrder().front().GetUUID();
		cell.CreateEntityWithUUID(shared, "Copy");
		REQUIRE(FileSystem::WriteText(project.Directory / "Assets" / "Worlds" / "Gen" / "Ground" / "1_0.stscene", ToText(SceneSerializer::Serialize(cell))));
		project.Context.GetAssetManager()->ReimportAsset(project.GetCell(1, 0));
		project.Context.GetAssetManager()->WaitForImports();

		const EditorCommandResult result = project.Try("world.cook", { { "world", UUIDToJson(project.World) } });
		REQUIRE_FALSE(result.Success);
		CHECK(result.Error.find(shared.ToString()) != std::string::npos);
		CHECK(result.Error.find("cell (Ground, 0, 1)") != std::string::npos);
		CHECK(result.Error.find("cell (Ground, 1, 0)") != std::string::npos);
		CHECK_FALSE(project.ReadWorld()->GetData().Index.has_value());
	}
}

TEST_SUITE("Editor.WorldCook")
{
	TEST_CASE("An export cooks the stale worlds, and its game starts with the fresh index and the cooked capacity")
	{
		WorldProject project("WorldCookExport", 3);
		project.Run("project.setStartScene", { { "scene", UUIDToJson(project.Persistent) } });
		CHECK_FALSE(project.ReadWorld()->GetData().Index.has_value());
		const std::filesystem::path output = project.Directory.parent_path() / "Build";
		const nlohmann::json exported = project.Run("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } });
		const Ref<WorldAsset> cooked = project.ReadWorld();
		REQUIRE(cooked->GetData().Index.has_value());
		CHECK(cooked->GetData().Index->Cells.size() == 9);
		const uint32_t recommended = cooked->GetData().Index->GetRecommendedMaxBodies();
		REQUIRE(recommended > 0);
		CHECK(project.Context.GetUndoStack().GetPosition() == 0); // Derived data: no undo step

		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		const Ref<WorldAsset> packed = game->GetAssetManager()->LoadAssetSync<WorldAsset>(project.World);
		REQUIRE(packed);
		REQUIRE(packed->GetData().Index.has_value());
		CHECK(packed->GetData().Index->MaxBodiesInRadius == cooked->GetData().Index->MaxBodiesInRadius);
		CHECK(game->GetScene()->GetSettings().PhysicsCapacity.MaxBodies == recommended);
		const IPhysicsCapacity* capacity = game->GetScene()->GetService<IPhysicsCapacity>();
		REQUIRE(capacity);
		CHECK(capacity->GetCapacityInfo().MaxBodies >= recommended);
	}
}
