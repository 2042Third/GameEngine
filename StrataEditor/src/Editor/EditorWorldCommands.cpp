#include "Editor/AssetEdit.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/AssetFileAction.h"
#include "Editor/EditorWorld.h"
#include "Editor/ProjectTemplates.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"
#include "Editor/WorldCellIO.h"
#include "Editor/WorldCook.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Physics/PhysicsWorld.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scene/SceneServices.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldComponents.h>
#include <Strata/World/WorldStreamer.h>

#include <nlohmann/json.hpp>

#include <chrono>

// Worlds and the physics that holds them: world.cook, world.info and physics.stats.

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;

		nlohmann::json CellKeyJson(const WorldData& data, const WorldCellKey& key)
		{
			const WorldLayer* layer = data.FindLayer(key.Layer);
			return { { "layer", layer ? nlohmann::json(layer->Name) : nlohmann::json(key.Layer) }, { "x", key.X }, { "z", key.Z } };
		}

		nlohmann::json DescribeStats(const WorldStreamingStats& stats)
		{
			return { { "wanted", stats.CellsWanted }, { "loading", stats.CellsLoading }, { "loaded", stats.CellsLoaded }, { "unloading", stats.CellsUnloading },
				{ "pinned", stats.CellsPinned }, { "failed", stats.CellsFailed } };
		}

		// world.status's answer: null without an open world.
		nlohmann::json DescribeWorldStatus(EditorContext& context, bool listCells, size_t limit)
		{
			const EditorWorld* world = context.GetWorld();
			if (!world)
				return { { "world", nullptr } };
			const WorldStreamer& streamer = world->GetStreamer();
			nlohmann::json cells = DescribeStats(streamer.GetStats());
			cells["dirty"] = world->GetDirtyCells().size();
			nlohmann::json result = {
				{ "world", UUIDToJson(world->GetWorld()) },
				{ "editLayer", world->GetEditLayer() },
				{ "visibleLayers", world->GetVisibleLayers() },
				{ "radiusScale", world->GetRadiusScale() },
				{ "persistentDirty", world->IsPersistentDirty() },
				{ "cells", std::move(cells) } };
			nlohmann::json sources = nlohmann::json::array();
			for (const WorldStreamingSource& source : streamer.GetSources())
				sources.push_back({ { "position", { source.Position.x, source.Position.y, source.Position.z } }, { "forward", { source.Forward.x, source.Forward.y, source.Forward.z } } });
			result["sources"] = std::move(sources);
			if (const Ref<WorldAsset> asset = world->GetWorldAsset())
			{
				result["name"] = asset->GetData().Name;
				if (listCells)
				{
					std::vector<WorldCellState> states;
					streamer.GetCellStates(states);
					nlohmann::json list = nlohmann::json::array();
					for (const WorldCellState& state : states)
					{
						if (state.Status == WorldCellStatus::Unloaded && !state.Wanted && !world->IsCellDirty(state.Scene))
							continue;
						if (list.size() >= limit)
							break;
						nlohmann::json cell = CellKeyJson(asset->GetData(), state.Key);
						cell["state"] = WorldCellStatusToString(state.Status);
						cell["entities"] = state.Entities;
						cell["dirty"] = world->IsCellDirty(state.Scene);
						cell["pinned"] = state.Pinned;
						cell["wanted"] = state.Wanted;
						if (!state.Error.empty())
							cell["error"] = state.Error;
						list.push_back(std::move(cell));
					}
					result["list"] = std::move(list);
				}
			}
			// While playing: the running game's streaming too.
			if (context.IsPlaying())
			{
				if (const IWorldStreaming* streaming = context.GetActiveScene()->GetService<IWorldStreaming>())
					result["playing"] = DescribeStats(streaming->GetStats());
			}
			return result;
		}

		// Cells named by key ({layer, x, z}), in a region ({center, radius} or {min: [x, z], max: [x, z]}) of the given layers;
		// false with the reason for invalid input.
		bool ReadCells(const EditorWorld& world, const nlohmann::json& parameters, std::vector<WorldCellKey>& outCells, std::string& outError)
		{
			const Ref<WorldAsset> asset = world.GetWorldAsset();
			if (!asset)
			{
				outError = "The world is still loading";
				return false;
			}
			const WorldData& data = asset->GetData();
			auto readLayer = [&](const nlohmann::json& value, uint8_t& out) -> bool
			{
				const WorldLayer* layer = value.is_string() ? data.FindLayer(value.get<std::string>()) : nullptr;
				if (!layer && value.is_number_unsigned() && value.get<uint64_t>() <= 255)
					layer = data.FindLayer(static_cast<uint8_t>(value.get<uint64_t>()));
				if (!layer)
				{
					outError = fmt::format("The world has no layer {}", value.dump());
					return false;
				}
				out = layer->ID;
				return true;
			};
			if (const nlohmann::json* cells = JsonUtils::Find(parameters, "cells"))
			{
				for (const nlohmann::json& cell : *cells)
				{
					WorldCellKey key;
					const nlohmann::json* layer = JsonUtils::Find(cell, "layer");
					const nlohmann::json* x = JsonUtils::Find(cell, "x");
					const nlohmann::json* z = JsonUtils::Find(cell, "z");
					if (!layer || !x || !z || !x->is_number_integer() || !z->is_number_integer() || !readLayer(*layer, key.Layer))
					{
						if (outError.empty())
							outError = "cells: each cell is {layer, x, z}";
						return false;
					}
					key.X = static_cast<int32_t>(std::clamp<int64_t>(x->get<int64_t>(), -WorldCellKey::c_MaxCoordinate, WorldCellKey::c_MaxCoordinate));
					key.Z = static_cast<int32_t>(std::clamp<int64_t>(z->get<int64_t>(), -WorldCellKey::c_MaxCoordinate, WorldCellKey::c_MaxCoordinate));
					if (asset->FindCell(key) < 0)
					{
						outError = fmt::format("The world has no cell {}", world.DescribeKey(key));
						return false;
					}
					outCells.push_back(key);
				}
			}
			if (const nlohmann::json* region = JsonUtils::Find(parameters, "region"))
			{
				std::vector<uint8_t> layers;
				if (const nlohmann::json* names = JsonUtils::Find(parameters, "layers"))
				{
					for (const nlohmann::json& name : *names)
					{
						uint8_t layer = 0;
						if (!readLayer(name, layer))
							return false;
						layers.push_back(layer);
					}
				}
				glm::dvec2 minimum(0.0);
				glm::dvec2 maximum(0.0);
				const nlohmann::json* center = JsonUtils::Find(*region, "center");
				const nlohmann::json* radius = JsonUtils::Find(*region, "radius");
				const nlohmann::json* min = JsonUtils::Find(*region, "min");
				const nlohmann::json* max = JsonUtils::Find(*region, "max");
				if (center && radius && center->is_array() && center->size() == 3 && radius->is_number() && radius->get<double>() >= 0.0)
				{
					const double r = radius->get<double>();
					minimum = glm::dvec2((*center)[0].get<double>() - r, (*center)[2].get<double>() - r);
					maximum = glm::dvec2((*center)[0].get<double>() + r, (*center)[2].get<double>() + r);
				}
				else if (min && max && min->is_array() && max->is_array() && min->size() == 2 && max->size() == 2)
				{
					minimum = glm::dvec2((*min)[0].get<double>(), (*min)[1].get<double>());
					maximum = glm::dvec2((*max)[0].get<double>(), (*max)[1].get<double>());
				}
				else
				{
					outError = "region: {center: [x, y, z], radius} or {min: [x, z], max: [x, z]}";
					return false;
				}
				for (const WorldCell& cell : data.Cells)
				{
					if (!layers.empty() && std::find(layers.begin(), layers.end(), cell.Layer) == layers.end())
						continue;
					const WorldCellSquare square = WorldGrid::CellBounds(*data.FindLayer(cell.Layer), data.GridOrigin, cell.X, cell.Z);
					if (square.Max.x >= minimum.x && square.Min.x <= maximum.x && square.Max.y >= minimum.y && square.Min.y <= maximum.y)
						outCells.push_back(cell.GetKey());
				}
			}
			return true;
		}

		nlohmann::json DescribeCells(const EditorWorld& world, const std::vector<WorldCellKey>& keys)
		{
			nlohmann::json cells = nlohmann::json::array();
			const Ref<WorldAsset> asset = world.GetWorldAsset();
			std::vector<WorldCellState> states;
			world.GetStreamer().GetCellStates(states);
			for (const WorldCellKey& key : keys)
			{
				nlohmann::json cell = CellKeyJson(asset->GetData(), key);
				const auto state = std::find_if(states.begin(), states.end(), [&](const WorldCellState& item) { return item.Key == key; });
				cell["state"] = state != states.end() ? WorldCellStatusToString(state->Status) : "Unloaded";
				cell["entities"] = state != states.end() ? state->Entities : 0;
				cells.push_back(std::move(cell));
			}
			return cells;
		}

		// world.create's and world.setLayers' layers: names (with the defaults) or objects {name, cellSize, streamingDistance,
		// enabledByDefault, loadBeforeStart}; IDs follow their order unless an object gives one.
		bool ReadLayers(const nlohmann::json& layers, float cellSize, std::vector<WorldLayer>& outLayers, std::string& outError)
		{
			if (!layers.is_array() || layers.empty())
			{
				outError = "layers: an array of layer names or {name, cellSize, streamingDistance, enabledByDefault, loadBeforeStart}";
				return false;
			}
			uint8_t nextID = 1;
			for (const nlohmann::json& item : layers)
			{
				WorldLayer layer;
				layer.ID = nextID;
				layer.CellSize = cellSize;
				layer.StreamingDistance = 4.0f * cellSize;
				if (item.is_string())
				{
					layer.Name = item.get<std::string>();
				}
				else if (item.is_object())
				{
					layer.Name = JsonUtils::GetString(item, "name");
					layer.ID = static_cast<uint8_t>(std::clamp<int64_t>(JsonUtils::GetInt(item, "id", nextID), 0, 255));
					layer.CellSize = JsonUtils::GetFloat(item, "cellSize", cellSize);
					layer.StreamingDistance = JsonUtils::GetFloat(item, "streamingDistance", 4.0f * layer.CellSize);
					layer.EnabledByDefault = JsonUtils::GetBool(item, "enabledByDefault", true);
					layer.LoadBeforeStart = JsonUtils::GetBool(item, "loadBeforeStart", outLayers.empty());
				}
				else
				{
					outError = "layers: an array of layer names or {name, cellSize, streamingDistance, enabledByDefault, loadBeforeStart}";
					return false;
				}
				if (item.is_string())
					layer.LoadBeforeStart = outLayers.empty(); // The first layer (the terrain) holds the ground a game starts on
				nextID = static_cast<uint8_t>(std::max<int>(nextID, layer.ID) + 1);
				outLayers.push_back(std::move(layer));
			}
			return true;
		}

		nlohmann::json DescribeLayers(const WorldData& data)
		{
			nlohmann::json layers = nlohmann::json::array();
			for (const WorldLayer& layer : data.Layers)
			{
				size_t cells = 0;
				for (const WorldCell& cell : data.Cells)
					cells += cell.Layer == layer.ID ? 1 : 0;
				layers.push_back({ { "id", layer.ID }, { "name", layer.Name }, { "cellSize", layer.CellSize }, { "streamingDistance", layer.StreamingDistance },
					{ "enabledByDefault", layer.EnabledByDefault }, { "loadBeforeStart", layer.LoadBeforeStart }, { "cells", cells } });
			}
			return layers;
		}

	}

	namespace CommandUtils
	{

		nlohmann::json DescribePhysicsStats(const Scene* scene)
		{
			if (!scene || !scene->IsRunning())
				return { { "running", false } };

			const PhysicsSystem* physics = scene->GetSystem<PhysicsSystem>();
			const PhysicsStats stats = physics ? physics->GetStats() : PhysicsStats();
			const PhysicsCapacityInfo capacity = physics ? physics->GetCapacityInfo() : PhysicsCapacityInfo();
			return {
				{ "running", true },
				{ "simulating", physics && physics->GetWorld() != nullptr },
				{ "bodies", {
					{ "total", stats.BodyCount },
					{ "static", stats.StaticBodyCount },
					{ "dynamic", stats.DynamicBodyCount },
					{ "kinematic", stats.KinematicBodyCount },
					{ "active", stats.ActiveBodyCount },
					{ "characters", stats.CharacterControllerCount },
					{ "slotsUsed", capacity.Bodies } } },
				{ "capacity", {
					{ "maxBodies", stats.MaxBodies },
					{ "maxBodyPairs", stats.MaxBodyPairs },
					{ "maxContactConstraints", stats.MaxContactConstraints },
					{ "reservedMB", static_cast<double>(capacity.ReservedBytes) / c_BytesPerMB } } },
				{ "waitingForSlot", capacity.WaitingForSlot },
				{ "pendingOther", capacity.PendingOther },
				{ "addedLastFrame", capacity.AddedLastUpdate },
				{ "removedLastFrame", capacity.RemovedLastUpdate },
				{ "changeMsLastFrame", stats.ChangeTime },
				{ "preparedSlices", stats.PreparedSlices },
				{ "growths", {
					{ "count", stats.GrowthCount },
					{ "lastMs", stats.LastGrowthMs },
					{ "from", stats.LastGrowthFrom },
					{ "to", stats.LastGrowthTo } } },
				{ "demand", {
					{ "bodies", stats.Demand.Bodies },
					{ "bodyPairs", stats.Demand.BodyPairs },
					{ "contactConstraints", stats.Demand.ContactConstraints } } },
				{ "contactOverflowSteps", stats.ContactOverflowSteps },
				{ "lastStepMs", stats.LastStepTime },
				{ "steps", stats.StepCount },
				{ "heldSteps", stats.HeldStepCount },
				{ "waitingForMeshes", stats.WaitingForMeshes } };
		}

	}

	void RegisterWorldCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "world.cook",
			"Measures a world's cells for its index, off the frame: per cell its entities, physics bodies, bounds and the stored bytes it loads, and "
			"the most bodies the persistent scene's streaming sources can keep loaded anywhere (maxBodiesInRadius); writes them into the world "
			"document. Only cells whose files changed since the last cook are read again (cellsRead; cellsReused). Fails, writing nothing, when "
			"cells share entity IDs with each other or with the persistent scene (the error names them) or a cell cannot be read. apply (default "
			"true): also sets the persistent scene's physics capacity (Physics Capacity > Max Bodies) to the recommended one, 1.25 times "
			"maxBodiesInRadius for spawns, when the persistent scene is the edited scene in edit mode (otherwise a warning says it was not set; an "
			"explicit apply: true then fails). One undo step. Returns staleBefore (cells measured again), maxBodies (the recommended capacity), "
			"seconds, and warnings: cells beyond the 20 km worlds are supported within, empty cells.",
			ObjectSchema({ { "world", AssetSchema("The world (.stworld): handle or path") },
				{ "apply", BoolSchema("Also set the persistent scene's physics capacity (default true)") } }, { "world" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				CommandArguments arguments(parameters);
				const AssetHandle world = ResolveAsset(context, arguments, "world", AssetType::World);
				const bool explicitApply = arguments.Has("apply");
				bool apply = arguments.GetBool("apply", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				const auto started = std::chrono::steady_clock::now();
				std::string error;
				Ref<WorldCook> cook(WorldCook::Start(*assets, world, context.GetWorldCookCache(), &error).release());
				if (!cook)
					return EditorCommandResult::Fail(error);
				const bool persistentOpen = !context.IsPlaying() && context.GetSceneHandle() == cook->GetCookedData().PersistentScene;
				if (apply && !persistentOpen)
				{
					if (explicitApply)
						return EditorCommandResult::InvalidParameters("apply sets the persistent scene's capacity: open the world's persistent scene (scene.open), "
							"in edit mode, first");
					apply = false;
				}
				const bool skippedApply = !explicitApply && !persistentOpen;
				return EditorCommandResult::Defer([cook, apply, skippedApply, started](EditorContext& pollContext) -> std::optional<EditorCommandResult>
				{
					if (!cook->Poll())
						return std::nullopt;
					if (!cook->Succeeded())
						return EditorCommandResult::Fail(cook->GetError());
					EditorAssetManager* assets = pollContext.GetAssetManager();
					if (!assets)
						return EditorCommandResult::Fail("The project was closed during the cook");
					const std::optional<AssetMetadata> metadata = assets->GetMetadata(cook->GetWorld());
					if (!metadata)
						return EditorCommandResult::Fail("The world was deleted during the cook");

					// The fresh index, and with apply the capacity: one undo step.
					std::string writeError;
					std::vector<uint8_t> before;
					if (!AssetEdit::ReadDocument(*assets, cook->GetWorld(), before, &writeError))
						return EditorCommandResult::Fail(writeError);
					const std::string text = JsonUtils::Dump(WorldAsset::ToDocument(cook->GetCookedData()), 1, '\t') + "\n";
					std::vector<uint8_t> after(text.begin(), text.end());
					Scope<EditorActionGroup> group = CreateScope<EditorActionGroup>(fmt::format("Cook {}", metadata->Name));
					bool recorded = false;
					if (before != after)
					{
						if (!AssetEdit::ApplyDocument(*assets, cook->GetWorld(), after, &writeError))
							return EditorCommandResult::Fail(writeError);
						group->Add(CreateScope<AssetDocumentAction>(*assets, cook->GetWorld(), metadata->Name, std::move(before), std::move(after)));
						recorded = true;
					}
					const WorldCookReport& report = cook->GetReport();
					bool applied = false;
					if (apply)
					{
						Scene& scene = *pollContext.GetEditScene();
						const SceneSettings previous = scene.GetSettings();
						SceneSettings settings = previous;
						settings.PhysicsCapacity.MaxBodies = report.RecommendedMaxBodies;
						if (settings.PhysicsCapacity != previous.PhysicsCapacity)
						{
							scene.GetSettings() = settings;
							group->Add(CreateScope<SceneSettingsAction>(scene, previous, settings));
							recorded = true;
						}
						applied = true;
					}
					if (recorded)
						pollContext.GetUndoStack().Record(std::move(group));
					std::vector<std::string> warnings = report.Warnings;
					if (skippedApply)
						warnings.push_back("The persistent scene is not the edited scene: its physics capacity was not set (open it and cook again)");
					return EditorCommandResult::Ok({
						{ "world", UUIDToJson(cook->GetWorld()) },
						{ "cells", cook->GetCookedData().Cells.size() },
						{ "staleBefore", report.CellsRead },
						{ "cellsRead", report.CellsRead },
						{ "cellsReused", report.CellsReused },
						{ "persistentBodies", report.Index.PersistentBodies },
						{ "maxBodiesInRadius", report.Index.MaxBodiesInRadius },
						{ "sources", report.SourceCount },
						{ "maxBodies", report.RecommendedMaxBodies },
						{ "recommendedMaxBodies", report.RecommendedMaxBodies },
						{ "reservedMB", static_cast<double>(report.ReservedBytes) / c_BytesPerMB },
						{ "applied", applied },
						{ "seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() },
						{ "warnings", std::move(warnings) } });
				});
			}, EditorCommandKind::Edit });

		registry.Register({ "world.create",
			"Makes a new world: <path>.stworld and its persistent scene <path>.stscene (the basic3d content, a \"World\" entity with its "
			"WorldComponent; the primary camera streams the cells). Layers default to Terrain, Buildings, Props and Gameplay, each of cellSize "
			"meters with a streaming distance of four cells (the first loads before a game starts); give names, or objects {name, cellSize, "
			"streamingDistance, enabledByDefault, loadBeforeStart}. open (default true) opens it (failing on unsaved changes unless force; a new "
			"history starts, like scene.open); without open, one undo step that removes the files. Returns {world, persistentScene, layers}.",
			ObjectSchema({ { "path", StringSchema("Path relative to the asset directory without extension, e.g. \"Worlds/Island\"") },
				{ "cellSize", NumberSchema("Side of the layers' cells in meters (1-65536, default 64)") },
				{ "layers", { { "type", "array" }, { "description", "Layer names, or {name, cellSize, streamingDistance, enabledByDefault, loadBeforeStart}" } } },
				{ "open", BoolSchema("Open the new world (default true)") },
				{ "force", ForceSchema() } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				CommandArguments arguments(parameters);
				std::string path = arguments.GetString("path");
				const bool open = arguments.GetBool("open", true);
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				double cellSize = 64.0;
				if (const nlohmann::json* size = JsonUtils::Find(parameters, "cellSize"))
				{
					if (!size->is_number() || size->get<double>() < WorldData::c_MinCellSize || size->get<double>() > WorldData::c_MaxCellSize)
						return EditorCommandResult::InvalidParameters("cellSize: a number of meters from 1 to 65536");
					cellSize = size->get<double>();
				}
				if (path.ends_with(".stworld"))
					path.resize(path.size() - std::string_view(".stworld").size());
				const std::filesystem::path relative = FileSystem::FromUTF8(path);
				if (path.empty() || relative.is_absolute() || path.find("..") != std::string::npos || relative.filename().empty())
					return EditorCommandResult::InvalidParameters("path: a path relative to the asset directory, e.g. \"Worlds/Island\"");
				const std::string worldPath = path + ".stworld";
				const std::string scenePath = path + ".stscene";
				for (const std::string& file : { worldPath, scenePath })
				{
					if (FileSystem::Exists(assets->GetAssetDirectory() / FileSystem::FromUTF8(file)))
						return EditorCommandResult::InvalidParameters(fmt::format("'{}' exists already", file));
				}

				WorldData data;
				data.Name = FileSystem::ToUTF8(relative.filename());
				std::string error;
				const nlohmann::json defaultLayers = { "Terrain", "Buildings", "Props", "Gameplay" };
				if (!ReadLayers(parameters.contains("layers") ? parameters["layers"] : defaultLayers, static_cast<float>(cellSize), data.Layers, error))
					return EditorCommandResult::InvalidParameters(error);
				const AssetHandle worldHandle;
				const AssetHandle sceneHandle;
				data.PersistentScene = sceneHandle;
				if (!data.Validate(&error))
					return EditorCommandResult::InvalidParameters(fmt::format("Invalid world: {}", error));
				if (open)
				{
					if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, fmt::format("creating and opening the world '{}'", path)))
						return std::move(*refused);
				}

				// The persistent scene and the world name each other: both handles are chosen first.
				Scene persistent(data.Name);
				ProjectTemplates::Populate(ProjectTemplates::c_Basic3D, persistent);
				persistent.CreateEntity("World").AddComponent<WorldComponent>().World = worldHandle;
				if (WorldCellIO::CreateDocument(*assets, scenePath, AssetType::Scene, sceneHandle, SceneSerializer::Serialize(persistent), &error) != sceneHandle)
					return EditorCommandResult::Fail(fmt::format("'{}' cannot be written: {}", scenePath, error));
				if (WorldCellIO::CreateDocument(*assets, worldPath, AssetType::World, worldHandle, WorldAsset::ToDocument(data), &error) != worldHandle)
				{
					TrashedAssets trashed;
					assets->MoveToTrash(scenePath, trashed);
					return EditorCommandResult::Fail(fmt::format("'{}' cannot be written: {}", worldPath, error));
				}
				nlohmann::json result = { { "world", UUIDToJson(worldHandle) }, { "persistentScene", UUIDToJson(sceneHandle) }, { "layers", DescribeLayers(data) } };
				if (!open)
				{
					Scope<EditorActionGroup> group = CreateScope<EditorActionGroup>(fmt::format("Create World {}", data.Name));
					group->Add(CreateScope<AssetCreateAction>(*assets, fmt::format("Create {}", scenePath), sceneHandle, scenePath));
					group->Add(CreateScope<AssetCreateAction>(*assets, fmt::format("Create {}", worldPath), worldHandle, worldPath));
					context.GetUndoStack().Record(std::move(group));
					return EditorCommandResult::Ok(std::move(result));
				}
				if (!context.BeginOpenScene(sceneHandle, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Defer([result, sceneHandle](EditorContext& pollContext) -> std::optional<EditorCommandResult>
				{
					if (pollContext.IsSceneLoading())
						return std::nullopt;
					if (pollContext.GetSceneHandle() != sceneHandle)
						return EditorCommandResult::Fail("The new world's persistent scene could not be opened (scene.loadStatus says why)");
					return EditorCommandResult::Ok(result);
				});
			} });

		registry.Register({ "world.status",
			"The world the editor edits (null when the edited scene is no world's persistent scene): its edit layer, visible layers and "
			"radius scale (world.edit), the source (the editor camera), its cells' counts (wanted, loading, loaded, unloading, pinned, failed, "
			"dirty: changed since saved) and whether the persistent scene is dirty; with cells: true, the cells that are not plain unloaded ones, at "
			"most limit (state, entities, dirty, pinned, wanted, error). While playing, playing has the running game's streaming counts.",
			ObjectSchema({ { "cells", BoolSchema("List the cells (default false)") }, { "limit", IntegerSchema("Most cells listed (default 1000)", 1, 100000) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool cells = arguments.GetBool("cells", false);
				const int64_t limit = arguments.GetInt("limit", 1000, 1, 100000);
				if (!arguments.IsValid())
					return arguments.Fail();
				return EditorCommandResult::Ok(DescribeWorldStatus(context, cells, static_cast<size_t>(limit)));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "world.edit",
			"How the editor edits the open world, for the session: editLayer, the layer new top-level entities go into (a layer's name, or "
			"\"Persistent\" for the persistent scene, the default); visibleLayers, the layers it streams (empty: all); radiusScale, a factor of "
			"every layer's streaming distance around the editor camera (0.1-10, default 1). Returns world.status.",
			ObjectSchema({ { "editLayer", StringSchema("A layer's name or \"Persistent\"") },
				{ "visibleLayers", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Layer names (empty: all)" } } },
				{ "radiusScale", NumberSchema("Factor of the layers' streaming distances (0.1-10)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorWorld* world = context.GetWorld();
				if (!world)
					return EditorCommandResult::Fail("The edited scene is no world's persistent scene (scene.open one, or world.create)");
				CommandArguments arguments(parameters);
				const std::string editLayer = arguments.GetString("editLayer", world->GetEditLayer());
				if (!arguments.IsValid())
					return arguments.Fail();
				std::optional<std::vector<std::string>> visible;
				if (const nlohmann::json* layers = JsonUtils::Find(parameters, "visibleLayers"))
				{
					if (!layers->is_array())
						return EditorCommandResult::InvalidParameters("visibleLayers: an array of layer names");
					visible.emplace();
					for (const nlohmann::json& layer : *layers)
					{
						if (!layer.is_string())
							return EditorCommandResult::InvalidParameters("visibleLayers: an array of layer names");
						visible->push_back(layer.get<std::string>());
					}
				}
				std::optional<float> radiusScale;
				if (const nlohmann::json* scale = JsonUtils::Find(parameters, "radiusScale"))
				{
					if (!scale->is_number() || scale->get<double>() < 0.1 || scale->get<double>() > 10.0)
						return EditorCommandResult::InvalidParameters("radiusScale: a number from 0.1 to 10");
					radiusScale = scale->get<float>();
				}
				// Checked before anything changes.
				const Ref<WorldAsset> asset = world->GetWorldAsset();
				if ((editLayer != EditorWorld::c_PersistentLayer || visible) && !asset)
					return EditorCommandResult::Fail("The world is still loading; try again in a moment");
				if (editLayer != EditorWorld::c_PersistentLayer && !asset->GetData().FindLayer(editLayer))
					return EditorCommandResult::InvalidParameters(fmt::format("The world has no layer '{}'", editLayer));
				if (visible)
				{
					for (const std::string& layer : *visible)
					{
						if (!asset->GetData().FindLayer(layer))
							return EditorCommandResult::InvalidParameters(fmt::format("The world has no layer '{}'", layer));
					}
				}
				world->SetEditLayer(editLayer);
				if (visible)
					world->SetVisibleLayers(std::move(*visible));
				if (radiusScale)
					world->SetRadiusScale(*radiusScale);
				return EditorCommandResult::Ok(DescribeWorldStatus(context, false, 0));
			}, EditorCommandKind::Session });

		registry.Register({ "world.loadCells",
			"Loads cells of the open world and keeps them (pinned) until unpinned: cells by key ([{layer, x, z}], layer by name or ID) or in a "
			"region ({center: [x, y, z], radius} or {min: [x, z], max: [x, z]}, of the given layers or all). pin (default true): false loads them "
			"without keeping them (they go when the camera leaves). unpin: cells to let go; unpinAll: let every pinned cell go. wait: answer once "
			"the cells are loaded (or failed). Returns {cells: [{layer, x, z, state, entities}]}.",
			ObjectSchema({ { "cells", { { "type", "array" }, { "description", "Cells: [{layer, x, z}]" } } },
				{ "region", AnyObjectSchema("{center: [x, y, z], radius} or {min: [x, z], max: [x, z]}") },
				{ "layers", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "The region's layers (default: all)" } } },
				{ "pin", BoolSchema("Keep the cells loaded (default true)") },
				{ "unpin", { { "type", "array" }, { "description", "Cells to let go: [{layer, x, z}]" } } },
				{ "unpinAll", BoolSchema("Let every pinned cell go") },
				{ "wait", BoolSchema("Answer once the cells are loaded") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorWorld* world = context.GetWorld();
				if (!world)
					return EditorCommandResult::Fail("The edited scene is no world's persistent scene (scene.open one, or world.create)");
				CommandArguments arguments(parameters);
				const bool pin = arguments.GetBool("pin", true);
				const bool unpinAll = arguments.GetBool("unpinAll", false);
				const bool wait = arguments.GetBool("wait", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::vector<WorldCellKey> cells;
				std::vector<WorldCellKey> unpinned;
				std::string error;
				if (!ReadCells(*world, parameters, cells, error))
					return EditorCommandResult::InvalidParameters(error);
				if (const nlohmann::json* unpin = JsonUtils::Find(parameters, "unpin"))
				{
					if (!ReadCells(*world, { { "cells", *unpin } }, unpinned, error))
						return EditorCommandResult::InvalidParameters(error);
				}
				if (unpinAll)
					world->UnpinAllCells();
				for (const WorldCellKey& key : unpinned)
					world->UnpinCell(key);
				// Loaded through a pin either way; without pin, the pin goes once they are in.
				for (const WorldCellKey& key : cells)
					world->PinCell(key);
				if (!wait && pin)
					return EditorCommandResult::Ok({ { "cells", DescribeCells(*world, cells) } });
				return EditorCommandResult::Defer([cells, pin, scene = world->GetWorld()](EditorContext& pollContext) -> std::optional<EditorCommandResult>
				{
					EditorWorld* current = pollContext.GetWorld();
					if (!current || current->GetWorld() != scene)
						return EditorCommandResult::Fail("The world was closed before its cells were in");
					std::vector<WorldCellState> states;
					current->GetStreamer().GetCellStates(states);
					for (const WorldCellKey& key : cells)
					{
						const auto state = std::find_if(states.begin(), states.end(), [&](const WorldCellState& item) { return item.Key == key; });
						if (state == states.end() || (state->Status != WorldCellStatus::Loaded && state->Status != WorldCellStatus::Failed))
							return std::nullopt;
					}
					if (!pin)
					{
						for (const WorldCellKey& key : cells)
							current->UnpinCell(key);
					}
					return EditorCommandResult::Ok({ { "cells", DescribeCells(*current, cells) } });
				});
			}, EditorCommandKind::Session });

		registry.Register({ "world.moveEntities",
			"Moves top-level entities of the open world into the cell of a layer at their positions (made when it does not exist yet, "
			"written at the next save), or into the persistent scene with layer \"Persistent\". Their descendants go with them. One undo "
			"step. Fails, changing nothing, for a child entity, an unknown layer or a target cell that is not loaded yet (it is loading then: "
			"try again once it is in).",
			ObjectSchema({ { "entities", EntityArraySchema("Top-level entities to move") },
				{ "layer", StringSchema("A layer's name, or \"Persistent\"") } }, { "entities", "layer" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorWorld* world = context.GetWorld();
				if (!world)
					return EditorCommandResult::Fail("The edited scene is no world's persistent scene (scene.open one, or world.create)");
				std::string error;
				if (!context.CheckSceneEditable(&error))
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				Scene& scene = *context.GetEditScene();
				const std::vector<Entity> entities = arguments.GetEntities(scene, "entities");
				const std::string layer = arguments.GetString("layer");
				if (!arguments.IsValid())
					return arguments.Fail();
				std::vector<UUID> ids;
				for (const Entity& entity : entities)
					ids.push_back(entity.GetUUID());
				SceneEditTransaction transaction(scene, fmt::format("Move {} to {}", SceneEdit::DescribeEntities(entities), layer), ids);
				if (!world->MoveEntities(entities, layer, &error))
				{
					transaction.Rollback();
					return EditorCommandResult::InvalidParameters(error);
				}
				context.CommitEdit(transaction);
				nlohmann::json moved = nlohmann::json::array();
				for (const Entity& entity : entities)
					moved.push_back(UUIDToJson(entity.GetUUID()));
				return EditorCommandResult::Ok({ { "entities", std::move(moved) }, { "layer", layer } });
			} });

		registry.Register({ "world.info",
			"Describes a world: its name, persistent scene and grid origin, its layers (id, name, cell size and streaming distance in meters, "
			"whether they stream by default and load before a game starts, their cells), its cells, and its index (cooked: whether it has one; "
			"staleCells: cells changed since it was made, which world.cook measures again; maxBodiesInRadius, persistentBodies and the recommended "
			"physics capacity). Distant cells are not loaded at all (no low-detail stand-ins): a game sets fog or far planes to match the layers' "
			"streaming distances.",
			ObjectSchema({ { "world", AssetSchema("The world (.stworld): handle or path") } }, { "world" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "world", AssetType::World);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::vector<uint8_t> bytes;
				std::string error;
				if (!assets->ReadStoredData(handle, bytes, &error))
					return EditorCommandResult::Fail(fmt::format("The world cannot be read: {}", error));
				const Ref<WorldAsset> world = WorldAsset::Deserialize(bytes, &error);
				if (!world)
					return EditorCommandResult::Fail(fmt::format("The world is invalid: {}", error));
				const WorldData& data = world->GetData();
				nlohmann::json index = { { "cooked", data.Index.has_value() } };
				if (data.Index)
				{
					size_t stale = 0;
					for (const WorldCell& cell : data.Cells)
					{
						const WorldCellSummary* summary = world->FindSummary(cell.GetKey());
						stale += !summary || summary->Stamp != fmt::format("{:016X}", assets->GetContentStamp(cell.Scene)) ? 1 : 0;
					}
					index["staleCells"] = stale;
					index["maxBodiesInRadius"] = data.Index->MaxBodiesInRadius;
					index["persistentBodies"] = data.Index->PersistentBodies;
					index["recommendedMaxBodies"] = data.Index->GetRecommendedMaxBodies();
				}
				return EditorCommandResult::Ok({
					{ "world", UUIDToJson(handle) },
					{ "name", data.Name },
					{ "persistentScene", data.PersistentScene.IsValid() ? UUIDToJson(data.PersistentScene) : nlohmann::json(nullptr) },
					{ "gridOrigin", { data.GridOrigin.x, data.GridOrigin.y } },
					{ "layers", DescribeLayers(data) },
					{ "cells", data.Cells.size() },
					{ "index", std::move(index) } });
			}, EditorCommandKind::Query });

		registry.Register({ "physics.stats",
			"The physics of the running game (play or simulate): bodies in the simulation (total, static, dynamic, kinematic, awake, character "
			"controllers) and the body slots in use, the world's capacities (maxBodies, maxBodyPairs, maxContactConstraints) and the memory they "
			"reserve (reservedMB), bodies waiting for a slot (only a world at its growth limit leaves bodies without collision; others grow before "
			"their next step) and waiting for other reasons (meshes loading, degenerate transforms), the bodies built and destroyed and the "
			"milliseconds spent on it in the last frame, the slices a switched-to scene's bodies were built in while it loaded (preparedSlices; 0 when they "
			"were built as the scene started), the world's growths (count, the last one's milliseconds and capacities), what the world "
			"needed beyond its capacities (demand), steps that dropped contacts, the last step's milliseconds, the steps run, the steps held while "
			"mesh colliders wait for their meshes, and whether the last step was held. {running: false} while editing; simulating is false while "
			"a running scene has no physics components.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				const Ref<Scene>& scene = context.GetActiveScene();
				return EditorCommandResult::Ok(DescribePhysicsStats(context.IsPlaying() ? scene.get() : nullptr));
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}

}
