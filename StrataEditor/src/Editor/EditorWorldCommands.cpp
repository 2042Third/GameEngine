#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Physics/PhysicsWorld.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneServices.h>

#include <nlohmann/json.hpp>

// Worlds and the physics that holds them: physics.stats (the world.* commands join it).

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;

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
