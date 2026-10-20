#include "World/WorldTestUtils.h"

#include "Strata/Asset/AssetPackCook.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
	#include "Strata/Physics/PhysicsWorld.h"
#endif

#include <spdlog/fmt/fmt.h>

#include <cmath>
#include <map>
#include <unordered_map>

namespace Strata::Tests
{

	namespace
	{

		constexpr uint64_t c_WorldHandle = 0x77000000;
		constexpr uint64_t c_PersistentHandle = 0x77000001;
		constexpr uint64_t c_GroundCellBase = 0x77100000;
		constexpr uint64_t c_PropsCellBase = 0x77200000;
		constexpr uint64_t c_GroundEntityBase = 0x7800000000ull;
		constexpr uint64_t c_PropsEntityBase = 0x7900000000ull;

		std::vector<uint8_t> ToBytes(const nlohmann::json& json)
		{
			const std::string text = JsonUtils::Dump(json);
			return std::vector<uint8_t>(text.begin(), text.end());
		}

		// The bodies a cell's entities make (PhysicsWorld::CountBodyCandidates); none in an engine without physics, whose boxes
		// get no colliders.
		uint64_t CountBodies(const Scene& scene)
		{
#if ST_WITH_PHYSICS
			return PhysicsWorld::CountBodyCandidates(scene);
#else
			(void)scene;
			return 0;
#endif
		}

		Entity CreateBox(Scene& scene, UUID id, const std::string& name, const glm::vec3& position, const glm::vec3& scale, bool collider)
		{
			Entity entity = scene.CreateEntityWithUUID(id, name);
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = position;
			transform.Scale = scale;
			entity.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
#if ST_WITH_PHYSICS
			if (collider)
				entity.AddComponent<BoxColliderComponent>();
#else
			(void)collider;
#endif
			return entity;
		}

	}

	AssetHandle GeneratedWorld::GetCellScene(uint8_t layer, int32_t x, int32_t z) const
	{
		for (const WorldCell& cell : Data.Cells)
		{
			if (cell.Layer == layer && cell.X == x && cell.Z == z)
				return cell.Scene;
		}
		return UUID::Null();
	}

	GeneratedWorld WriteGeneratedWorld(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification)
	{
		std::string error;
		std::optional<GeneratedWorld> world = TryWriteGeneratedWorld(directory, specification, &error);
		REQUIRE_MESSAGE(world, error);
		return std::move(*world);
	}

	std::optional<GeneratedWorld> TryWriteGeneratedWorld(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification, std::string* outError)
	{
		GeneratedWorld world;
		world.World = UUID(c_WorldHandle);
		world.Persistent = UUID(c_PersistentHandle);
		world.PackPath = directory / "World.stpak";

		WorldData& data = world.Data;
		data.Name = specification.Name;
		data.PersistentScene = world.Persistent;
		data.GridOrigin = specification.GridOrigin;
		data.Layers.push_back(WorldLayer { 1, "Ground", specification.CellSize, specification.StreamingDistance, true, specification.GroundLoadBeforeStart });
		if (specification.PropsPerCell > 0)
			data.Layers.push_back(WorldLayer { 2, "Props", specification.CellSize * 0.5f, specification.PropsStreamingDistance, true, false });
		WorldIndex index;

		std::map<uint64_t, std::vector<uint8_t>> documents; // Handle -> stored form
		std::vector<AssetPackSource> sources;
		auto addScene = [&](AssetHandle handle, const std::string& path, std::vector<uint8_t> document)
		{
			AssetPackSource source;
			source.Metadata.Handle = handle;
			source.Metadata.Type = AssetType::Scene;
			source.Metadata.Path = path;
			source.Metadata.Name = path;
			source.Metadata.StoredSize = document.size();
			sources.push_back(std::move(source));
			documents[static_cast<uint64_t>(handle)] = std::move(document);
		};

		// The Ground cells, row by row.
		const WorldLayer& ground = data.Layers[0];
		const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(std::max(specification.EntitiesPerCell, 1u)))));
		auto buildGroundCell = [&specification, side, &data, ground](uint32_t x, uint32_t z, uint32_t cellIndex, Scene& cell)
		{
			const WorldCellSquare square = WorldGrid::CellBounds(ground, data.GridOrigin, static_cast<int32_t>(x), static_cast<int32_t>(z));
			const glm::vec3 minimum(static_cast<float>(square.Min.x), 0.0f, static_cast<float>(square.Min.y));
			const float spacing = specification.CellSize / static_cast<float>(side);
			const uint64_t entityBase = c_GroundEntityBase + static_cast<uint64_t>(cellIndex) * 0x10000;
			for (uint32_t entity = 0; entity < specification.EntitiesPerCell; entity++)
			{
				const glm::vec3 position = minimum + glm::vec3((static_cast<float>(entity % side) + 0.5f) * spacing, 0.5f, (static_cast<float>(entity / side) + 0.5f) * spacing);
				CreateBox(cell, UUID(entityBase + entity), fmt::format("Box {}_{} #{}", x, z, entity), position, glm::vec3(0.5f), entity < specification.BodiesPerCell);
			}
			if (specification.Floors)
			{
				const glm::vec3 center = minimum + glm::vec3(specification.CellSize * 0.5f, -0.5f, specification.CellSize * 0.5f);
				CreateBox(cell, UUID(entityBase + 0xFFFF), fmt::format("Floor {}_{}", x, z), center, glm::vec3(specification.CellSize, 1.0f, specification.CellSize), true);
			}
		};
		// Cells made as the pack is written: their coordinates by handle.
		struct LaterCell
		{
			uint32_t X = 0;
			uint32_t Z = 0;
			uint32_t Index = 0;
		};
		std::unordered_map<uint64_t, LaterCell> laterCells;
		uint32_t cellIndex = 0;
		for (uint32_t z = 0; z < specification.GridZ; z++)
		{
			for (uint32_t x = 0; x < specification.GridX; x++, cellIndex++)
			{
				const WorldCellSquare square = WorldGrid::CellBounds(ground, data.GridOrigin, static_cast<int32_t>(x), static_cast<int32_t>(z));
				const glm::vec3 minimum(static_cast<float>(square.Min.x), 0.0f, static_cast<float>(square.Min.y));
				const AssetHandle handle = UUID(c_GroundCellBase + cellIndex);
				const std::string path = fmt::format("Worlds/{}/Ground/{}_{}.stscene", specification.Name, x, z);
				WorldCellSummary summary;
				summary.Layer = ground.ID;
				summary.X = static_cast<int32_t>(x);
				summary.Z = static_cast<int32_t>(z);
				summary.Stamp = "generated";
				summary.BoundsMin = minimum + glm::vec3(0.0f, -1.0f, 0.0f);
				summary.BoundsMax = minimum + glm::vec3(specification.CellSize, 1.0f, specification.CellSize);
				if (specification.GenerateCellsWhenPacked)
				{
					// What the cell will hold, counted as CreateBox makes it (colliders only with the physics module).
					const uint32_t floors = specification.Floors ? 1u : 0u;
					summary.Entities = specification.EntitiesPerCell + floors;
					summary.Bodies = ST_WITH_PHYSICS ? std::min(specification.BodiesPerCell, specification.EntitiesPerCell) + floors : 0u;
					laterCells.emplace(static_cast<uint64_t>(handle), LaterCell { x, z, cellIndex });
					AssetPackSource source;
					source.Metadata.Handle = handle;
					source.Metadata.Type = AssetType::Scene;
					source.Metadata.Path = path;
					source.Metadata.Name = path;
					// An estimate (the pack writer weighs its progress by it): the document is made as it is written.
					source.Metadata.StoredSize = static_cast<uint64_t>(summary.Entities) * 256;
					sources.push_back(std::move(source));
				}
				else
				{
					Scene cell(fmt::format("{} Ground {}_{}", specification.Name, x, z));
					buildGroundCell(x, z, cellIndex, cell);
					const bool broken = std::find(specification.BrokenCells.begin(), specification.BrokenCells.end(), cellIndex) != specification.BrokenCells.end();
					std::vector<uint8_t> document = broken ? std::vector<uint8_t> { 'n', 'o', 't', ' ', 'a', ' ', 's', 'c', 'e', 'n', 'e' } : ToBytes(SceneSerializer::Serialize(cell));
					summary.Entities = cell.GetEntityCount();
					summary.Bodies = CountBodies(cell);
					summary.ClosureBytes = document.size();
					addScene(handle, path, std::move(document));
				}
				world.BodiesPerGroundCell = summary.Bodies;
				index.Cells.push_back(summary);
				data.Cells.push_back(WorldCell { ground.ID, static_cast<int32_t>(x), static_cast<int32_t>(z), handle });
			}
		}
		world.GroundCells = cellIndex;

		// The Props cells: twice as many per side, a few renderers each.
		if (specification.PropsPerCell > 0)
		{
			const WorldLayer& props = data.Layers[1];
			uint32_t propIndex = 0;
			for (uint32_t z = 0; z < specification.GridZ * 2; z++)
			{
				for (uint32_t x = 0; x < specification.GridX * 2; x++, propIndex++)
				{
					const WorldCellSquare square = WorldGrid::CellBounds(props, data.GridOrigin, static_cast<int32_t>(x), static_cast<int32_t>(z));
					Scene cell(fmt::format("{} Props {}_{}", specification.Name, x, z));
					for (uint32_t prop = 0; prop < specification.PropsPerCell; prop++)
					{
						const glm::vec3 position(static_cast<float>(square.Min.x) + props.CellSize * (static_cast<float>(prop) + 0.5f) / static_cast<float>(specification.PropsPerCell), 1.5f,
							static_cast<float>(square.Min.y) + props.CellSize * 0.5f);
						CreateBox(cell, UUID(c_PropsEntityBase + static_cast<uint64_t>(propIndex) * 0x10000 + prop), fmt::format("Prop {}_{} #{}", x, z, prop), position, glm::vec3(0.25f), false);
					}
					const AssetHandle handle = UUID(c_PropsCellBase + propIndex);
					data.Cells.push_back(WorldCell { props.ID, static_cast<int32_t>(x), static_cast<int32_t>(z), handle });
					addScene(handle, fmt::format("Worlds/{}/Props/{}_{}.stscene", specification.Name, x, z), ToBytes(SceneSerializer::Serialize(cell)));
				}
			}
		}

		Scene persistent("Persistent");
		Entity camera = persistent.CreateEntityWithUUID(UUID(0x7700000000ull), "Camera");
		camera.AddComponent<CameraComponent>().Primary = true;
		if (specification.BuildPersistent)
			specification.BuildPersistent(persistent, world.World);
		index.PersistentBodies = CountBodies(persistent);
		// Every cell at once: more than any radius holds, which is what world.cook would bound it by.
		index.MaxBodiesInRadius = index.PersistentBodies;
		for (const WorldCellSummary& summary : index.Cells)
			index.MaxBodiesInRadius += summary.Bodies;
		if (specification.Index)
			data.Index = std::move(index);
		addScene(world.Persistent, "Scenes/Persistent.stscene", ToBytes(SceneSerializer::Serialize(persistent)));
		// Its WorldComponent references the world, as a pack's table records it.
		if (specification.BuildPersistent)
			sources.back().Dependencies.push_back(world.World);

		if (!data.Validate(outError))
			return std::nullopt;
		AssetPackSource worldSource;
		worldSource.Metadata.Handle = world.World;
		worldSource.Metadata.Type = AssetType::World;
		worldSource.Metadata.Path = fmt::format("Worlds/{}.stworld", specification.Name);
		worldSource.Metadata.Name = specification.Name;
		worldSource.Dependencies.push_back(world.Persistent);
		for (const WorldCell& cell : data.Cells)
			worldSource.Dependencies.push_back(cell.Scene);
		sources.push_back(std::move(worldSource));
		documents[c_WorldHandle] = ToBytes(WorldAsset::ToDocument(data));

		if (!AssetPackWriter::Write(world.PackPath, sources, [&](const AssetMetadata& metadata, std::vector<uint8_t>& outData, std::string* outWriteError)
			{
				if (const auto later = laterCells.find(static_cast<uint64_t>(metadata.Handle)); later != laterCells.end())
				{
					Scene cell(fmt::format("{} Ground {}_{}", specification.Name, later->second.X, later->second.Z));
					buildGroundCell(later->second.X, later->second.Z, later->second.Index, cell);
					outData = ToBytes(SceneSerializer::Serialize(cell));
				}
				else
				{
					outData = documents.at(static_cast<uint64_t>(metadata.Handle));
				}
				// Cooked as an export cooks them (world-io's binary form); a broken cell stays garbage.
				if (specification.CookDocuments && metadata.Type == AssetType::Scene
					&& std::find(specification.BrokenCells.begin(), specification.BrokenCells.end(), static_cast<uint32_t>(static_cast<uint64_t>(metadata.Handle) - c_GroundCellBase)) == specification.BrokenCells.end())
				{
					AssetPackCookOptions cookOptions;
					cookOptions.Cook = true;
					std::vector<AssetHandle> references;
					return CookForPack(metadata, outData, cookOptions, references, outWriteError);
				}
				return true;
			}, {}, nullptr, outError))
			return std::nullopt;
		return world;
	}

	std::filesystem::path WriteGeneratedWorldGame(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification, GeneratedWorld* outWorld)
	{
		std::string error;
		const std::optional<std::filesystem::path> path = TryWriteGeneratedWorldGame(directory, specification, outWorld, &error);
		REQUIRE_MESSAGE(path, error);
		return *path;
	}

	std::optional<std::filesystem::path> TryWriteGeneratedWorldGame(const std::filesystem::path& directory, const GeneratedWorldSpecification& specification,
		GeneratedWorld* outWorld, std::string* outError)
	{
		std::optional<GeneratedWorld> world = TryWriteGeneratedWorld(directory, specification, outError);
		if (!world)
			return std::nullopt;
		GameManifest manifest;
		manifest.Name = specification.Name;
		manifest.AssetPack = "World.stpak";
		manifest.StartScene = world->Persistent;
		const std::filesystem::path path = directory / "World.stgame";
		if (!manifest.Save(path, outError))
			return std::nullopt;
		if (outWorld)
			*outWorld = std::move(*world);
		return path;
	}

	MountedWorld::MountedWorld(const std::string& name, const GeneratedWorldSpecification& specification)
		: World(WriteGeneratedWorld(CreateTemporaryDirectory(name), specification))
	{
		std::string error;
		Manager = RuntimeAssetManager::Create(World.PackPath, &error);
		REQUIRE_MESSAGE(Manager, error);
		Active = CreateScope<ScopedActiveManager>(Manager);
	}

}
