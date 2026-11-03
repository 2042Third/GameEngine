#include <doctest/doctest.h>

#include "Scene/InterpolationTestUtils.h"
#include "TestHelpers.h"
#include "Strata/Math/Math.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

// Interpolation of what is drawn (Scene::SetInterpolation, Scene/SceneInterpolation.cpp): what the fixed steps move is drawn
// between its poses before and after them, one step behind the clock, and the simulation is untouched.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A binary fraction of a second, so that frames of 1/1024 s keep step counts exact.
	constexpr float c_Step = 1.0f / 64.0f;
	// The Third-Person robot's run (m/s).
	constexpr float c_Speed = 6.5f;

	void SetTestStep(Scene& scene)
	{
		scene.GetSettings().FixedTimestep = c_Step;
		scene.GetSettings().MaxFixedStepsPerFrame = 8;
	}

	glm::vec3 RenderedPosition(Scene& scene, Entity entity)
	{
		return glm::vec3(GetRenderedTransform(scene, entity)[3]);
	}

	// Steps the scene with frames of `frame` seconds until a frame runs a fixed step (returns after that frame).
	void RunUntilStep(Scene& scene, float frame)
	{
		const uint64_t steps = scene.GetFixedStepCount();
		for (int guard = 0; guard < 10000 && scene.GetFixedStepCount() == steps; guard++)
			scene.OnUpdateRuntime(Timestep(frame));
		REQUIRE(scene.GetFixedStepCount() > steps);
	}

	struct Increment
	{
		float Frame = 0.0f;
		float Distance = 0.0f;
	};

	// Every frame's increment along x of where a renderer draws an entity moved by fixed steps at c_Speed, from the frame after
	// the first stepping frame on.
	std::vector<Increment> MeasureIncrements(bool interpolate, const std::vector<float>& frames)
	{
		ScopedFixedMover moverScope;
		Scene scene("Increments");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		scene.SetInterpolation(interpolate);
		scene.OnRuntimeStart();
		std::vector<Increment> increments;
		std::optional<float> previous;
		for (const float frame : frames)
		{
			scene.OnUpdateRuntime(Timestep(frame));
			const float x = RenderedPosition(scene, body).x;
			if (previous)
				increments.push_back(Increment { frame, x - *previous });
			if (previous || scene.GetFixedStepCount() > 0)
				previous = x;
		}
		scene.OnRuntimeStop();
		return increments;
	}

	// The roles of the set by entity.
	std::optional<InterpolationRole> FindRole(const Scene& scene, Entity entity)
	{
		const std::span<const entt::entity> entities = scene.GetInterpolatedEntities();
		for (size_t slot = 0; slot < entities.size(); slot++)
		{
			if (entities[slot] == entity.GetHandle())
				return scene.GetInterpolatedEntries()[slot].Role;
		}
		return std::nullopt;
	}

	bool NearMatrix(const glm::mat4& a, const glm::mat4& b, float tolerance)
	{
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (!(std::abs(a[column][row] - b[column][row]) <= tolerance))
					return false;
			}
		}
		return true;
	}

	void WriteTranslation(Scene& scene, Entity entity, const glm::vec3& translation)
	{
		scene.GetRegistry().patch<TransformComponent>(entity.GetHandle(), [&](TransformComponent& transform) { transform.Translation = translation; });
	}

	void WriteRotation(Scene& scene, Entity entity, const glm::quat& rotation)
	{
		scene.GetRegistry().patch<TransformComponent>(entity.GetHandle(), [&](TransformComponent& transform) { transform.Rotation = rotation; });
	}

}

TEST_SUITE("Scene.Timing")
{
	TEST_CASE("An entity moved only by fixed steps is drawn moving every frame at 1000 frames per second")
	{
		// The owner's case: 64 steps per second, 1,024 frames per second.
		const std::vector<float> uniform(1000, 1.0f / 1024.0f);
		const std::vector<Increment> interpolated = MeasureIncrements(true, uniform);
		REQUIRE(interpolated.size() > 900);
		uint32_t wrong = 0;
		for (const Increment& increment : interpolated)
		{
			if (!(std::abs(increment.Distance - c_Speed * increment.Frame) <= 1.0e-4f) || !(increment.Distance > 0.0f))
				wrong++;
		}
		CHECK(wrong == 0);

		// Frame times that vary as a real frame rate's do.
		std::mt19937 random(8);
		std::uniform_real_distribution<float> length(0.0005f, 0.0015f);
		std::vector<float> varying(1000);
		for (float& frame : varying)
			frame = length(random);
		wrong = 0;
		for (const Increment& increment : MeasureIncrements(true, varying))
		{
			if (!(std::abs(increment.Distance - c_Speed * increment.Frame) <= 1.0e-4f))
				wrong++;
		}
		CHECK(wrong == 0);

		// Without interpolation the same measure sees the owner's jitter: still on 15 frames of 16, then a whole step at once.
		const std::vector<Increment> stepped = MeasureIncrements(false, uniform);
		uint32_t still = 0;
		uint32_t jumps = 0;
		for (const Increment& increment : stepped)
		{
			if (increment.Distance == 0.0f)
				still++;
			else if (std::abs(increment.Distance - c_Speed * c_Step) <= 1.0e-4f)
				jumps++;
		}
		CHECK(still + jumps == stepped.size());
		CHECK(still >= 14 * jumps);
		CHECK(jumps >= 60);
	}

	TEST_CASE("The alpha draws one step behind the clock")
	{
		ScopedFixedMover moverScope;
		Scene scene("Alpha");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();

		std::vector<float> frames;
		for (const float milliseconds : { 1.0f, 4.17f, 6.94f, 8.33f, 16.67f, 33.3f })
			frames.insert(frames.end(), 120, milliseconds / 1000.0f);
		frames.push_back(0.2f); // A hitch: 8 steps run, the rest of the backlog is dropped
		frames.insert(frames.end(), 60, 0.001f);

		// The accumulator, mirrored with the loop's own operations in float (no time dilation).
		float accumulator = 0.0f;
		uint32_t lastSteps = 1;
		std::optional<float> previousDrawnTime;
		for (const float frame : frames)
		{
			const uint64_t before = scene.GetFixedStepCount();
			scene.OnUpdateRuntime(Timestep(frame));
			const uint32_t ran = static_cast<uint32_t>(scene.GetFixedStepCount() - before);
			accumulator += frame;
			uint32_t steps = 0;
			while (accumulator >= c_Step && steps < 8)
			{
				accumulator -= c_Step;
				steps++;
			}
			if (steps == 8 && accumulator > c_Step)
				accumulator = c_Step;
			REQUIRE(steps == ran);
			if (ran > 0)
				lastSteps = ran;
			if (scene.GetFixedStepCount() == 0)
				continue;
			const float expected = std::clamp(1.0f - (1.0f - std::min(accumulator / c_Step, 1.0f)) / static_cast<float>(lastSteps), 0.0f, 1.0f);
			CHECK(std::abs(scene.GetStepAlpha() - expected) <= 1.0e-6f);

			// The drawn time T + a - h, recovered from the linear mover, never decreases.
			const float drawnTime = RenderedPosition(scene, body).x / c_Speed;
			const float simulated = static_cast<float>(scene.GetFixedStepCount()) * c_Step;
			CHECK(std::abs(drawnTime - (simulated + accumulator - c_Step)) <= 2.0e-5f);
			if (previousDrawnTime)
				CHECK(drawnTime >= *previousDrawnTime - 1.0e-6f);
			previousDrawnTime = drawnTime;
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Roots blend, descendants compose")
	{
		ScopedFixedMover moverScope;
		Scene scene("Roles");
		SetTestStep(scene);
		Entity root = scene.CreateEntity("Root");
		Entity child = scene.CreateChildEntity(root, "Child");
		Entity grandchild = scene.CreateChildEntity(child, "Grandchild");
		grandchild.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.5f, 0.0f);
		AddFixedMover(scene, root, glm::vec3(c_Speed, 0.0f, 1.0f), 2.0f);
		// The child is written every frame in OnUpdate, as PlaceAttachments writes an attachment under its animator.
		float time = 0.0f;
		FixedMoverSystem::UpdateHook = [&](Scene& updated, Timestep timestep)
		{
			time += timestep;
			updated.GetRegistry().patch<TransformComponent>(child.GetHandle(), [&](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.3f, 1.0f + 0.1f * std::sin(time * 20.0f), 0.0f);
				transform.Rotation = glm::angleAxis(time * 3.0f, glm::vec3(1.0f, 0.0f, 0.0f));
			});
		};
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();

		uint32_t composed = 0;
		uint32_t between = 0;
		for (int frame = 0; frame < 200; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			scene.UpdateDrawnTransforms();
			const glm::mat4* rootDrawn = scene.FindDrawnTransform(root.GetHandle());
			const glm::mat4* childDrawn = scene.FindDrawnTransform(child.GetHandle());
			const glm::mat4* grandchildDrawn = scene.FindDrawnTransform(grandchild.GetHandle());
			if (!rootDrawn)
				continue;
			REQUIRE(childDrawn);
			REQUIRE(grandchildDrawn);
			CHECK(FindRole(scene, root) == InterpolationRole::Blend);
			CHECK(FindRole(scene, child) == InterpolationRole::Compose);
			CHECK(FindRole(scene, grandchild) == InterpolationRole::Compose);
			CHECK(SameMatrix(*childDrawn, Math::ComposeWorldTransform(*rootDrawn, child.GetComponent<TransformComponent>().GetTransform())));
			CHECK(SameMatrix(*grandchildDrawn, Math::ComposeWorldTransform(*childDrawn, grandchild.GetComponent<TransformComponent>().GetTransform())));
			composed++;
			if (!SameMatrix(*rootDrawn, scene.GetWorldTransform(root)))
				between++;
		}
		CHECK(composed > 150);
		CHECK(between > 150);
		const InterpolationStats stats = scene.GetInterpolationStats();
		CHECK(stats.Entities == 3);
		CHECK(stats.Blended == 1);
		CHECK(stats.Composed == 2);
		CHECK(scene.ValidateInterpolation());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Per-frame writes")
	{
		ScopedFixedMover moverScope;
		Scene scene("Writes");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		Entity holder = scene.CreateEntity("Holder");
		Entity carried = scene.CreateChildEntity(holder, "Carried");
		Entity rider = scene.CreateChildEntity(body, "Rider");
		Entity other = scene.CreateEntity("Other");
		other.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 5.0f);
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		AddFixedMover(scene, carried, glm::vec3(0.0f, 0.0f, c_Speed));
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		RunUntilStep(scene, 1.0f / 1024.0f);
		RunUntilStep(scene, 1.0f / 1024.0f);
		scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		REQUIRE(FindRole(scene, body) == InterpolationRole::Blend);
		REQUIRE(FindRole(scene, carried) == InterpolationRole::Blend);
		REQUIRE(FindRole(scene, rider) == InterpolationRole::Compose);

		SUBCASE("A translation of a blended entity draws it where it was put until the next step")
		{
			WriteTranslation(scene, body, scene.GetWorldTransform(body)[3] + glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(SameMatrix(scene.GetDrawnTransform(body), scene.GetWorldTransform(body)));
			CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
			CHECK(FindRole(scene, body) == InterpolationRole::Exact);
			const uint64_t steps = scene.GetFixedStepCount();
			for (int frame = 0; frame < 8 && scene.GetFixedStepCount() == steps; frame++)
			{
				scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
				if (scene.GetFixedStepCount() == steps)
					CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
			}
		}
		SUBCASE("A turn keeps the translation blending and is drawn as written")
		{
			const glm::mat4 before = GetRenderedTransform(scene, body);
			const glm::quat turn = glm::angleAxis(0.7f, glm::vec3(0.0f, 1.0f, 0.0f));
			WriteRotation(scene, body, turn);
			const glm::mat4 pending = scene.GetDrawnTransform(body);
			const glm::mat4 drawn = GetRenderedTransform(scene, body);
			CHECK(FindRole(scene, body) == InterpolationRole::Blend);
			CHECK(NearMatrix(pending, drawn, 1.0e-5f));
			CHECK(std::abs(drawn[3].x - before[3].x) <= 1.0e-5f);
			CHECK(drawn[3].x < scene.GetWorldTransform(body)[3].x);
			const glm::mat3 written = glm::mat3_cast(turn);
			CHECK(NearMatrix(glm::mat4(glm::mat3(drawn)), glm::mat4(written), 1.0e-5f));
			// The next frames keep the turn and go on blending the translation.
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			const glm::mat4 next = GetRenderedTransform(scene, body);
			CHECK(next[3].x > drawn[3].x);
			CHECK(NearMatrix(glm::mat4(glm::mat3(next)), glm::mat4(written), 1.0e-5f));
		}
		SUBCASE("A translation of an ancestor draws a blended descendant where it is")
		{
			WriteTranslation(scene, holder, glm::vec3(0.0f, 2.0f, 0.0f));
			CHECK(SameMatrix(GetRenderedTransform(scene, carried), scene.GetWorldTransform(carried)));
			CHECK(FindRole(scene, carried) == InterpolationRole::Exact);
		}
		SUBCASE("A reparent draws a composed entity where it is")
		{
			REQUIRE(scene.SetParent(rider, other, true));
			CHECK(SameMatrix(scene.GetDrawnTransform(rider), scene.GetWorldTransform(rider)));
			CHECK(SameMatrix(GetRenderedTransform(scene, rider), scene.GetWorldTransform(rider)));
			CHECK(FindRole(scene, rider) == InterpolationRole::Exact);
		}
		SUBCASE("A write to a composed entity changes only its local transform")
		{
			WriteTranslation(scene, rider, glm::vec3(0.0f, 1.0f, 0.0f));
			const glm::mat4 drawn = GetRenderedTransform(scene, rider);
			CHECK(FindRole(scene, rider) == InterpolationRole::Compose);
			CHECK(SameMatrix(drawn, Math::ComposeWorldTransform(*scene.FindDrawnTransform(body.GetHandle()), rider.GetComponent<TransformComponent>().GetTransform())));
		}
		CHECK(scene.ValidateInterpolation());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Entities created during the steps")
	{
		ScopedFixedMover moverScope;
		Scene scene("Spawns");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		Entity spawnedChild;
		Entity spawnedRoot;
		uint64_t spawnStep = 3;
		FixedMoverSystem::FixedHook = [&](Scene& updated, float)
		{
			if (updated.GetFixedStepCount() != spawnStep)
				return;
			spawnedChild = updated.CreateChildEntity(body, "SpawnedChild");
			spawnedChild.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
			spawnedRoot = updated.CreateEntity("SpawnedRoot");
			spawnedRoot.GetComponent<TransformComponent>().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		};
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		while (!spawnedChild)
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		// The frame that spawned them, then one without a step.
		for (int frame = 0; frame < 2; frame++)
		{
			CHECK(FindRole(scene, spawnedChild) == InterpolationRole::Compose);
			CHECK(FindRole(scene, spawnedRoot) == InterpolationRole::Exact);
			const glm::mat4 childDrawn = GetRenderedTransform(scene, spawnedChild);
			CHECK(SameMatrix(childDrawn,
				Math::ComposeWorldTransform(*scene.FindDrawnTransform(body.GetHandle()), spawnedChild.GetComponent<TransformComponent>().GetTransform())));
			CHECK(std::abs(childDrawn[3].y - 1.0f) <= 1.0e-6f);
			CHECK(SameMatrix(GetRenderedTransform(scene, spawnedRoot), scene.GetWorldTransform(spawnedRoot)));
			CHECK(GetRenderedTransform(scene, spawnedRoot)[3].x == 10.0f);
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Leaving the set")
	{
		ScopedFixedMover moverScope;
		Scene scene("Leaving");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		Entity doomed = scene.CreateEntity("Doomed");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		AddFixedMover(scene, doomed, glm::vec3(0.0f, c_Speed, 0.0f));
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		RunUntilStep(scene, 1.0f / 1024.0f);
		RunUntilStep(scene, 1.0f / 1024.0f);
		scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		REQUIRE(scene.FindDrawnTransform(body.GetHandle()) == nullptr); // Not current before a renderer's update
		REQUIRE(!SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));

		// A destroyed entity is skipped; one that takes its index is not in the set.
		const entt::entity doomedHandle = doomed.GetHandle();
		scene.DestroyEntity(doomed);
		Entity successor = scene.CreateEntity("Successor");
		REQUIRE(entt::to_entity(successor.GetHandle()) == entt::to_entity(doomedHandle));
		CHECK(scene.FindDrawnTransform(successor.GetHandle()) == nullptr);
		CHECK(SameMatrix(scene.GetDrawnTransform(successor), scene.GetWorldTransform(successor)));
		CHECK(scene.ValidateInterpolation());

		// A body that stops is drawn where it is from the next stepping frame, and readers of the change log hear of it.
		RemoveFixedMover(scene, body);
		const uint64_t version = scene.GetTransformsVersion();
		RunUntilStep(scene, 1.0f / 1024.0f);
		CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
		CHECK(scene.FindDrawnTransform(body.GetHandle()) == nullptr);
		std::vector<entt::entity> changes;
		REQUIRE(scene.GetWorldTransformChanges(version, changes));
		CHECK(std::find(changes.begin(), changes.end(), body.GetHandle()) != changes.end());
		CHECK(scene.GetInterpolatedEntities().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Pause, step, resume, time scale")
	{
		ScopedFixedMover moverScope;
		Scene scene("Pause");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		RunUntilStep(scene, 1.0f / 1024.0f);
		for (int frame = 0; frame < 5; frame++)
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		float drawn = RenderedPosition(scene, body).x;
		REQUIRE(drawn < scene.GetWorldTransform(body)[3].x);

		// Paused: exact at once, and in stepped frames.
		scene.SetPaused(true);
		CHECK(scene.GetInterpolatedEntities().empty());
		CHECK(scene.GetStepAlpha() == 1.0f);
		CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
		scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
		scene.Step(2);
		for (int frame = 0; frame < 2; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
			CHECK(scene.GetStepAlpha() == 1.0f);
		}

		// Resumed: the drawn pose never moves backward.
		scene.SetPaused(false);
		drawn = RenderedPosition(scene, body).x;
		for (int frame = 0; frame < 100; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			const float next = RenderedPosition(scene, body).x;
			CHECK(next >= drawn);
			drawn = next;
		}

		// Time scale 0 freezes the alpha and the drawn pose; 0.1 draws uniform slow motion.
		scene.SetTimeScale(0.0f);
		scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		const float frozenAlpha = scene.GetStepAlpha();
		const glm::mat4 frozen = GetRenderedTransform(scene, body);
		const uint64_t frozenVersion = scene.GetDrawnTransformsVersion();
		for (int frame = 0; frame < 20; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			CHECK(scene.GetStepAlpha() == frozenAlpha);
			CHECK(SameMatrix(GetRenderedTransform(scene, body), frozen));
		}
		CHECK(scene.GetDrawnTransformsVersion() == frozenVersion);
		scene.SetTimeScale(0.1f);
		drawn = RenderedPosition(scene, body).x;
		for (int frame = 0; frame < 400; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			const float next = RenderedPosition(scene, body).x;
			CHECK(std::abs((next - drawn) - c_Speed * 0.1f / 1024.0f) <= 1.0e-5f);
			drawn = next;
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("Breaks")
	{
		ScopedFixedMover moverScope;
		Scene scene("Breaks");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		Entity child = scene.CreateChildEntity(body, "Child");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		AddFixedMover(scene, child, glm::vec3(0.0f, 1.0f, 0.0f));
		bool breakInStep = false;
		bool jumpInStep = false;
		FixedMoverSystem::FixedHook = [&](Scene& updated, float)
		{
			if (breakInStep)
				updated.BreakInterpolation(body);
			if (jumpInStep)
			{
				body.GetComponent<TransformComponent>().Translation.x += 10.0f;
				updated.MarkTransformChanged(body);
			}
		};
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		RunUntilStep(scene, 1.0f / 1024.0f);
		RunUntilStep(scene, 1.0f / 1024.0f);
		scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		REQUIRE(FindRole(scene, child) == InterpolationRole::Blend);

		SUBCASE("Outside the steps: the entity and what blends below it are drawn where they are")
		{
			REQUIRE(!SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
			const uint64_t version = scene.GetTransformsVersion();
			scene.BreakInterpolation(body);
			CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
			CHECK(SameMatrix(GetRenderedTransform(scene, child), scene.GetWorldTransform(child)));
			std::vector<entt::entity> changes;
			REQUIRE(scene.GetWorldTransformChanges(version, changes));
			CHECK(changes.size() == 2);
		}
		SUBCASE("Inside the steps: the poses they leave are drawn exactly")
		{
			breakInStep = true;
			RunUntilStep(scene, 1.0f / 1024.0f);
			breakInStep = false;
			CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
			CHECK(SameMatrix(GetRenderedTransform(scene, child), scene.GetWorldTransform(child)));
			// The next step blends again.
			RunUntilStep(scene, 1.0f / 1024.0f);
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			CHECK(!SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
		}
		SUBCASE("A step faster than box3d's maximum speed is a teleport")
		{
			jumpInStep = true;
			RunUntilStep(scene, 1.0f / 1024.0f);
			jumpInStep = false;
			CHECK(FindRole(scene, body) == InterpolationRole::Exact);
			CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
		}
		CHECK(scene.ValidateInterpolation());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Off by default")
	{
		ScopedFixedMover moverScope;
		Scene scene("Bare");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		Entity child = scene.CreateChildEntity(body, "Child");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f), 1.0f);
		CHECK(!scene.IsInterpolationEnabled());
		scene.OnRuntimeStart();
		CHECK(!scene.IsInterpolating());
		for (int frame = 0; frame < 100; frame++)
		{
			scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
			scene.UpdateDrawnTransforms();
			CHECK(scene.GetInterpolatedEntities().empty());
			CHECK(scene.GetStepAlpha() == 1.0f);
			CHECK(scene.FindDrawnTransform(body.GetHandle()) == nullptr);
			CHECK(SameMatrix(scene.GetDrawnTransform(body), scene.GetWorldTransform(body)));
			CHECK(SameMatrix(scene.GetDrawnTransform(child), scene.GetWorldTransform(child)));
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("A fixed timestep change clears the set")
	{
		ScopedFixedMover moverScope;
		Scene scene("Rate");
		SetTestStep(scene);
		Entity body = scene.CreateEntity("Body");
		AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f));
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		RunUntilStep(scene, 1.0f / 1024.0f);
		REQUIRE(!scene.GetInterpolatedEntities().empty());
		scene.GetSettings().FixedTimestep = 1.0f / 16.0f;
		const uint64_t version = scene.GetDrawnTransformsVersion();
		scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
		CHECK(scene.GetInterpolatedEntities().empty());
		CHECK(scene.GetDrawnTransformsVersion() != version);
		CHECK(SameMatrix(GetRenderedTransform(scene, body), scene.GetWorldTransform(body)));
		// The new rate's steps build a new set.
		RunUntilStep(scene, 1.0f / 1024.0f);
		CHECK(!scene.GetInterpolatedEntities().empty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Mirrored roots are drawn mirrored and turning between their steps, also in their first segment")
	{
		// A root's pose keeps its authored signs where the decomposed matrix before the steps has its reflection folded into X:
		// the first segment after an entity starts moving takes its From from that matrix and its To from the entity's pose.
		for (const glm::vec3& scale : { glm::vec3(1.0f, 1.0f, -1.0f), glm::vec3(-1.0f, -1.0f, 1.0f), glm::vec3(-1.0f, 2.0f, -0.5f) })
		{
			INFO("scale ", scale.x, " ", scale.y, " ", scale.z);
			ScopedFixedMover moverScope;
			Scene scene("Mirrored");
			SetTestStep(scene);
			Entity body = scene.CreateEntity("Body");
			body.GetComponent<TransformComponent>().Rotation = glm::angleAxis(0.4f, glm::vec3(0.0f, 1.0f, 0.0f));
			body.GetComponent<TransformComponent>().Scale = scale;
			body.MarkModified<TransformComponent>();
			AddFixedMover(scene, body, glm::vec3(c_Speed, 0.0f, 0.0f), 3.0f, glm::normalize(glm::vec3(0.2f, 1.0f, 0.1f)));
			scene.SetInterpolation(true);
			scene.OnRuntimeStart();

			// The entity's own pose before the latest stepping frame's steps and after them: the drawn pose lies between.
			TransformComponent from = body.GetComponent<TransformComponent>();
			TransformComponent to = from;
			uint32_t segments = 0;
			uint32_t between = 0;
			uint32_t wrong = 0;
			for (int frame = 0; frame < 200; frame++)
			{
				const uint64_t steps = scene.GetFixedStepCount();
				const TransformComponent before = body.GetComponent<TransformComponent>();
				scene.OnUpdateRuntime(Timestep(1.0f / 1024.0f));
				if (scene.GetFixedStepCount() != steps)
				{
					from = before;
					to = body.GetComponent<TransformComponent>();
					segments++;
				}
				if (segments == 0)
					continue;
				const float alpha = scene.GetStepAlpha();
				const glm::quat target = glm::dot(from.Rotation, to.Rotation) < 0.0f ? -to.Rotation : to.Rotation;
				const glm::quat rotation = glm::normalize(from.Rotation * (1.0f - alpha) + target * alpha);
				const glm::mat4 expected = Math::ComposeTransform(from.Translation + (to.Translation - from.Translation) * alpha, rotation, scale);
				const glm::mat4 drawn = GetRenderedTransform(scene, body);
				// Mirrored as the entity is and never squashed: the determinant stays the scale's product.
				const bool mirrored = std::abs(glm::determinant(glm::mat3(drawn)) - scale.x * scale.y * scale.z) <= 1.0e-3f;
				wrong += NearMatrix(drawn, expected, 1.0e-4f) && mirrored ? 0 : 1;
				between += alpha < 1.0f ? 1 : 0;
			}
			CHECK(wrong == 0);
			CHECK(segments > 10);
			CHECK(between > 150);
			CHECK(FindRole(scene, body) == InterpolationRole::Blend);
			CHECK(scene.ValidateInterpolation());
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("A running scene without entities captures nothing on its stepping frames")
	{
		// Play of a new, empty scene: the capture runs on every stepping frame, before any entity created the storages it reads.
		ScopedFixedMover moverScope;
		Scene scene("Empty");
		SetTestStep(scene);
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		CHECK(std::as_const(scene.GetRegistry()).storage<HierarchyComponent>() == nullptr);
		for (int frame = 0; frame < 64; frame++)
			scene.OnUpdateRuntime(Timestep(1.0f / 256.0f));
		CHECK(scene.GetFixedStepCount() == 16);
		CHECK(scene.IsInterpolating());
		CHECK(scene.GetInterpolatedEntities().empty());
		CHECK(scene.ValidateInterpolation());
		scene.UpdateDrawnTransforms();
		scene.OnRuntimeStop();
	}
}

namespace
{

	// The twin scenes of the sweep: the same entities and edits, one interpolating.
	struct SweepWorld
	{
		Scene* Target = nullptr;
		std::vector<entt::entity> Entities;
		std::mt19937 Random { 77 };
	};

	Entity PickEntity(SweepWorld& world, uint32_t pick)
	{
		for (size_t attempt = 0; attempt < world.Entities.size(); attempt++)
		{
			const entt::entity handle = world.Entities[(pick + attempt) % world.Entities.size()];
			if (world.Target->GetRegistry().valid(handle))
				return Entity(handle, world.Target);
		}
		return Entity();
	}

	void CheckDrawnTransforms(Scene& scene)
	{
		scene.UpdateDrawnTransforms();
		const entt::registry& registry = scene.GetRegistry();
		const std::span<const entt::entity> entities = scene.GetInterpolatedEntities();
		const std::span<const InterpolatedEntry> entries = scene.GetInterpolatedEntries();
		const std::span<const glm::mat4> drawn = scene.GetDrawnMatrices();
		uint32_t wrong = 0;
		for (size_t slot = 0; slot < entities.size(); slot++)
		{
			if (!registry.valid(entities[slot]))
				continue;
			const glm::mat4& world = registry.get<WorldTransformComponent>(entities[slot]).Matrix;
			const InterpolatedEntry& entry = entries[slot];
			if (entry.Role == InterpolationRole::Exact || (entry.Role == InterpolationRole::Blend && scene.GetStepAlpha() >= 1.0f))
			{
				wrong += SameMatrix(drawn[slot], world) ? 0 : 1;
			}
			else if (entry.Role == InterpolationRole::Compose)
			{
				// Through the engine's composition, as UpdateDrawnTransforms composes: an inlined product may round differently.
				const glm::mat4 expected = Math::ComposeWorldTransform(drawn[entry.ParentSlot], registry.get<TransformComponent>(entities[slot]).GetTransform());
				wrong += SameMatrix(drawn[slot], expected) ? 0 : 1;
			}
			else
			{
				// On the segment between the two poses.
				const glm::vec3 from = entry.From.Translation;
				const glm::vec3 segment = entry.To.Translation - from;
				const glm::vec3 point(drawn[slot][3]);
				const float length2 = glm::dot(segment, segment);
				const float along = length2 > 0.0f ? std::clamp(glm::dot(point - from, segment) / length2, 0.0f, 1.0f) : 0.0f;
				wrong += glm::length(point - (from + segment * along)) <= 1.0e-4f * std::max(1.0f, glm::length(from)) ? 0 : 1;
			}
		}
		CHECK(wrong == 0);
		std::string error;
		CHECK_MESSAGE(scene.ValidateInterpolation(&error), error);
	}

}

// Random frames and edits: a sample in the Scene shard, more frames in StrataTests.Fuzz (Tests::SelectSweepIterations).
TEST_SUITE("Scene.Timing.Sweep")
{
	TEST_CASE("Random frames, edits, steps, pauses, spawns and breaks leave the simulation as without interpolation and every drawn transform consistent")
	{
		ScopedFixedMover moverScope;
		Scene interpolated("Interpolated");
		Scene plain("Plain");
		SweepWorld worlds[2];
		worlds[0].Target = &interpolated;
		worlds[1].Target = &plain;
		std::mt19937 random(2026);
		std::uniform_real_distribution<float> unit(0.0f, 1.0f);
		const auto chance = [&](float probability) { return unit(random) < probability; };

		// 300 entities in hierarchies up to depth 3, the same in both scenes.
		std::vector<std::pair<int, glm::vec3>> layout;
		for (int index = 0; index < 300; index++)
		{
			int parent = -1;
			if (index > 0 && chance(0.6f))
			{
				parent = static_cast<int>(random() % static_cast<uint32_t>(index));
				if (interpolated.GetDepth(Entity(worlds[0].Entities[static_cast<size_t>(parent)], &interpolated)) >= 2)
					parent = -1;
			}
			const glm::vec3 position(unit(random) * 20.0f - 10.0f, unit(random) * 4.0f, unit(random) * 20.0f - 10.0f);
			for (SweepWorld& world : worlds)
			{
				Entity entity = parent < 0 ? world.Target->CreateEntity("E") : world.Target->CreateChildEntity(Entity(world.Entities[static_cast<size_t>(parent)], world.Target), "E");
				entity.GetComponent<TransformComponent>().Translation = position;
				world.Entities.push_back(entity.GetHandle());
			}
		}
		for (int index = 0; index < 300; index++)
		{
			if (!chance(0.4f))
				continue;
			const glm::vec3 velocity(unit(random) * 8.0f - 4.0f, unit(random) * 2.0f - 1.0f, unit(random) * 8.0f - 4.0f);
			const float turn = unit(random) * 6.0f - 3.0f;
			for (SweepWorld& world : worlds)
				AddFixedMover(*world.Target, Entity(world.Entities[static_cast<size_t>(index)], world.Target), velocity, turn);
		}
		// Inside the steps, each scene draws from its own generator: both run the same steps, so both draw the same.
		FixedMoverSystem::FixedHook = [&worlds](Scene& scene, float)
		{
			SweepWorld& world = &scene == worlds[0].Target ? worlds[0] : worlds[1];
			const uint32_t roll = world.Random() % 1000;
			const uint32_t pick = static_cast<uint32_t>(world.Random());
			Entity entity = PickEntity(world, pick);
			if (!entity)
				return;
			if (roll < 20)
			{
				Entity spawned = scene.CreateChildEntity(entity, "Spawned");
				spawned.GetComponent<TransformComponent>().Translation = glm::vec3(0.5f, 0.5f, 0.0f);
				world.Entities.push_back(spawned.GetHandle());
			}
			else if (roll < 30)
			{
				Entity spawned = scene.CreateEntity("SpawnedRoot");
				spawned.GetComponent<TransformComponent>().Translation = glm::vec3(1.0f, 2.0f, 3.0f);
				world.Entities.push_back(spawned.GetHandle());
			}
			else if (roll < 35)
			{
				scene.DestroyEntity(entity);
			}
			else if (roll < 55)
			{
				scene.BreakInterpolation(entity);
			}
			else if (roll < 65)
			{
				entity.GetComponent<TransformComponent>().Translation.y += 20.0f; // A jump the speed net catches
				scene.MarkTransformChanged(entity);
			}
		};
		interpolated.SetInterpolation(true);
		interpolated.OnRuntimeStart();
		plain.OnRuntimeStart();

		const float frameLengths[] = { 0.0002f, 0.001f, 1.0f / 1024.0f, 0.004f, 0.007f, 1.0f / 60.0f, 0.025f, 0.04f };
		const uint32_t frames = Tests::SelectSweepIterations(2000, 20000);
		uint32_t mismatches = 0;
		uint32_t blendedFrames = 0;
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			// Edits between frames (commands, the editor), the same in both scenes.
			const uint32_t pick = static_cast<uint32_t>(random());
			const float roll = unit(random);
			const glm::vec3 offset(unit(random) - 0.5f, unit(random) - 0.5f, unit(random) - 0.5f);
			const uint32_t other = static_cast<uint32_t>(random());
			const float frameLength = chance(0.5f) ? frameLengths[random() % 8] : 0.0002f + unit(random) * 0.0398f;
			const uint32_t pauseRoll = static_cast<uint32_t>(random() % 1000);
			const float timeScales[] = { 0.0f, 0.1f, 1.0f, 1.0f, 2.0f };
			const float timeScale = timeScales[random() % 5];
			for (SweepWorld& world : worlds)
			{
				Scene& scene = *world.Target;
				Entity entity = PickEntity(world, pick);
				if (!entity)
					continue;
				if (roll < 0.15f)
					WriteTranslation(scene, entity, entity.GetComponent<TransformComponent>().Translation + offset);
				else if (roll < 0.3f)
					WriteRotation(scene, entity, glm::normalize(entity.GetComponent<TransformComponent>().Rotation * glm::angleAxis(offset.x, glm::vec3(0.0f, 1.0f, 0.0f))));
				else if (roll < 0.33f)
					scene.SetParent(entity, PickEntity(world, other), true);
				else if (roll < 0.35f)
					scene.SetParent(entity, Entity(), true);
				else if (roll < 0.37f)
					scene.DestroyEntity(entity);
				else if (roll < 0.40f)
					world.Entities.push_back(scene.CreateChildEntity(entity, "Created").GetHandle());
				else if (roll < 0.43f)
					scene.BreakInterpolation(entity);
				if (pauseRoll < 15)
					scene.SetPaused(!scene.IsPaused());
				else if (pauseRoll < 40 && scene.IsPaused())
					scene.Step(1);
				else if (pauseRoll < 50)
					scene.SetTimeScale(timeScale);
			}
			// The drawn transform the next update will draw, asked before it (pending writes): what a script sees.
			Entity probe = PickEntity(worlds[0], pick);
			if (probe)
			{
				const glm::mat4 pending = interpolated.GetDrawnTransform(probe);
				interpolated.UpdateDrawnTransforms();
				const glm::mat4* cached = interpolated.FindDrawnTransform(probe.GetHandle());
				const glm::mat4 current = cached ? *cached : interpolated.GetWorldTransform(probe);
				if (!NearMatrix(pending, current, 1.0e-4f * std::max(1.0f, glm::length(glm::vec3(current[3])))))
					mismatches++;
			}

			for (SweepWorld& world : worlds)
				world.Target->OnUpdateRuntime(Timestep(frameLength));

			// The simulation is the same with and without interpolation, bit for bit.
			REQUIRE(interpolated.GetFixedStepCount() == plain.GetFixedStepCount());
			REQUIRE(worlds[0].Entities == worlds[1].Entities);
			const entt::registry& left = interpolated.GetRegistry();
			const entt::registry& right = plain.GetRegistry();
			uint32_t different = 0;
			for (const entt::entity handle : worlds[0].Entities)
			{
				if (left.valid(handle) != right.valid(handle))
				{
					different++;
					continue;
				}
				if (!left.valid(handle))
					continue;
				const TransformComponent& a = left.get<TransformComponent>(handle);
				const TransformComponent& b = right.get<TransformComponent>(handle);
				if (std::memcmp(&a, &b, sizeof(TransformComponent)) != 0
					|| !SameMatrix(left.get<WorldTransformComponent>(handle).Matrix, right.get<WorldTransformComponent>(handle).Matrix))
					different++;
			}
			REQUIRE(different == 0);
			CheckDrawnTransforms(interpolated);
			if (interpolated.GetInterpolationStats().Blended > 0 && interpolated.GetStepAlpha() < 1.0f)
				blendedFrames++;
		}
		CHECK(mismatches == 0);
		CHECK(blendedFrames > frames / 4);
		interpolated.OnRuntimeStop();
		plain.OnRuntimeStop();
	}
}
