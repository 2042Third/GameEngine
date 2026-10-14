#include "Editor/AssetEdit.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"
#include "Editor/WorldCook.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Physics/PhysicsWorld.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneServices.h>
#include <Strata/World/WorldAsset.h>

#include <nlohmann/json.hpp>

// Worlds and the physics that holds them: world.cook, world.info and physics.stats.

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;

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
			"cells share entity IDs with each other or with the persistent scene (the error names them) or a cell cannot be read. apply: also "
			"sets the persistent scene's physics capacity (Physics Capacity > Max Bodies) to the recommended one, 1.25 times maxBodiesInRadius for "
			"spawns; the persistent scene must be the edited scene, in edit mode. One undo step. warnings: cells beyond the 20 km worlds are "
			"supported within, and empty cells.",
			ObjectSchema({ { "world", AssetSchema("The world (.stworld): handle or path") },
				{ "apply", BoolSchema("Also set the persistent scene's physics capacity (default false)") } }, { "world" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				EditorAssetManager* assets = context.GetAssetManager();
				if (!assets)
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				CommandArguments arguments(parameters);
				const AssetHandle world = ResolveAsset(context, arguments, "world", AssetType::World);
				const bool apply = arguments.GetBool("apply", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				Ref<WorldCook> cook(WorldCook::Start(*assets, world, context.GetWorldCookCache(), &error).release());
				if (!cook)
					return EditorCommandResult::Fail(error);
				if (apply && (context.IsPlaying() || context.GetSceneHandle() != cook->GetCookedData().PersistentScene))
					return EditorCommandResult::InvalidParameters("apply sets the persistent scene's capacity: open the world's persistent scene (scene.open), "
						"in edit mode, first");
				return EditorCommandResult::Defer([cook, apply](EditorContext& pollContext) -> std::optional<EditorCommandResult>
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
					return EditorCommandResult::Ok({
						{ "world", UUIDToJson(cook->GetWorld()) },
						{ "cells", cook->GetCookedData().Cells.size() },
						{ "cellsRead", report.CellsRead },
						{ "cellsReused", report.CellsReused },
						{ "persistentBodies", report.Index.PersistentBodies },
						{ "maxBodiesInRadius", report.Index.MaxBodiesInRadius },
						{ "sources", report.SourceCount },
						{ "recommendedMaxBodies", report.RecommendedMaxBodies },
						{ "reservedMB", static_cast<double>(report.ReservedBytes) / c_BytesPerMB },
						{ "applied", applied },
						{ "warnings", report.Warnings } });
				});
			}, EditorCommandKind::Edit });

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
