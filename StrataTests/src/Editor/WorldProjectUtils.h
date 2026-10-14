#pragma once

#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldComponents.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// A project with a generated world for the editor's world tests (Editor.World, Editor.WorldCook).

namespace Strata::Tests
{

	inline constexpr uint64_t c_GenWorldHandle = 0x6E0000000001;
	inline constexpr uint64_t c_GenPersistentHandle = 0x6E0000000002;
	inline constexpr float c_GenCellSize = 32.0f;
	inline constexpr float c_GenSourceRadius = 40.0f;

	inline std::string ToDocumentText(const nlohmann::json& json)
	{
		return JsonUtils::Dump(json, 1, '\t') + "\n";
	}

	inline void WriteProjectAsset(const std::filesystem::path& assets, const std::string& path, const nlohmann::json& document, uint64_t handle, const char* type)
	{
		const std::filesystem::path file = assets / FileSystem::FromUTF8(path);
		REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
		REQUIRE(FileSystem::WriteText(file, ToDocumentText(document)));
		const nlohmann::json meta = { { "Handle", UUID(handle).ToString() }, { "ImportSettings", nlohmann::json::object() },
			{ "Strata", { { "Format", "AssetMeta" }, { "Version", 1 } } }, { "Type", type } };
		REQUIRE(FileSystem::WriteText(FileSystem::FromUTF8(FileSystem::ToUTF8(file) + ".meta"), ToDocumentText(meta)));
	}

	// A new project with a generated world "Gen": `side` x `side` Ground cells of 32 m, cell (x, z) holding
	// 7 + x renderers of which (3 x + 5 z) % 7 are static boxes (uneven, so that the hysteresis changes the busiest radius),
	// and a persistent scene with the WorldComponent, a source of radius 40 m and a floor (one body).
	struct WorldProject
	{
		std::filesystem::path Directory;
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		AssetHandle World = UUID(c_GenWorldHandle);
		AssetHandle Persistent = UUID(c_GenPersistentHandle);
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
			data.Layers.push_back(WorldLayer { 1, "Ground", c_GenCellSize, 48.0f, true, true });
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
						transform.Translation = glm::vec3(x * c_GenCellSize + 4.0f + 7.0f * static_cast<float>(index % 4), 0.5f + static_cast<float>(index),
							z * c_GenCellSize + 3.0f + 5.0f * static_cast<float>(index / 4));
						transform.Scale = glm::vec3(0.5f);
						entity.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
						if (index < bodies)
							entity.AddComponent<BoxColliderComponent>();
					}
					WriteProjectAsset(assets, fmt::format("Worlds/Gen/Ground/{}_{}.stscene", x, z), SceneSerializer::Serialize(*cell), static_cast<uint64_t>(handle), "Scene");
					data.Cells.push_back(WorldCell { 1, static_cast<int32_t>(x), static_cast<int32_t>(z), handle });
					CellScenes[{ x, z }] = cell.get();
					Cells.push_back(std::move(cell));
				}
			}
			Scene persistent("GenPersistent");
			persistent.CreateEntityWithUUID(UUID(0x6E0000000100ull), "World").AddComponent<WorldComponent>().World = World;
			Entity source = persistent.CreateEntityWithUUID(UUID(0x6E0000000101ull), "Source");
			source.AddComponent<StreamingSourceComponent>().Radius = c_GenSourceRadius;
			persistent.CreateEntityWithUUID(UUID(0x6E0000000102ull), "Floor").AddComponent<BoxColliderComponent>();
			WriteProjectAsset(assets, "Scenes/GenPersistent.stscene", SceneSerializer::Serialize(persistent), c_GenPersistentHandle, "Scene");
			WriteProjectAsset(assets, "Worlds/Gen.stworld", WorldAsset::ToDocument(data), c_GenWorldHandle, "World");

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

}
