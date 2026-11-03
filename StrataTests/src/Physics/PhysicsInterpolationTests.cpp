#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Scene/InterpolationTestUtils.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// Interpolation of what physics moves (Scene::SetInterpolation): characters and bodies are drawn between their last two
// fixed poses, smooth at any frame rate, while the simulation stays what it is without interpolation. Teleports are drawn
// where they land.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr float c_Step = 1.0f / 64.0f;

	Entity CreateWalker(Scene& scene, const glm::vec3& position)
	{
		Entity entity = scene.CreateEntity("Walker");
		entity.GetTransform().Translation = position;
		CharacterControllerComponent& controller = entity.AddComponent<CharacterControllerComponent>();
		controller.Height = 1.8f;
		controller.Radius = 0.3f;
		return entity;
	}

	struct ScreenSample
	{
		double Time = 0.0;
		glm::dvec2 Position = glm::dvec2(0.0);
	};

	// The audit's jitter: each sample's distance from the centered 100 ms average of the series, p99 - p1, the larger of the two
	// screen axes (pixels).
	double MeasureJitter(const std::vector<ScreenSample>& samples)
	{
		std::vector<double> residuals[2];
		size_t first = 0;
		size_t last = 0;
		glm::dvec2 sum(0.0);
		for (const ScreenSample& sample : samples)
		{
			// The window [t - 50 ms, t + 50 ms], kept by two moving ends.
			while (last < samples.size() && samples[last].Time <= sample.Time + 0.05)
				sum += samples[last++].Position;
			while (samples[first].Time < sample.Time - 0.05)
				sum -= samples[first++].Position;
			if (sample.Time - 0.05 < samples.front().Time || sample.Time + 0.05 > samples.back().Time)
				continue;
			const glm::dvec2 residual = sample.Position - sum / static_cast<double>(last - first);
			residuals[0].push_back(residual.x);
			residuals[1].push_back(residual.y);
		}
		double jitter = 0.0;
		for (std::vector<double>& axis : residuals)
		{
			REQUIRE(axis.size() > 10);
			std::sort(axis.begin(), axis.end());
			const double low = axis[axis.size() / 100];
			const double high = axis[axis.size() - 1 - axis.size() / 100];
			jitter = std::max(jitter, high - low);
		}
		return jitter;
	}

	// The owner's case: a character running at 6.5 m/s across the view of a camera that follows it as the Third-Person
	// FollowCamera does (its pivot toward the drawn feet + 1.5 m at a sharpness of 14/s, a 6.5 m arm, yaw 90, pitch -18), its
	// chest (the drawn feet + 1 m) projected to 3840x2160 at 60 degrees. Returns the jitter of the last 1.5 s of 2 s.
	double MeasureCharacterJitter(bool interpolate, float frame)
	{
		ScopedFixedMover moverScope;
		Scene scene("Run");
		scene.GetSettings().FixedTimestep = c_Step;
		CreateGround(scene);
		Entity walker = CreateWalker(scene, glm::vec3(0.0f, 0.0f, 20.0f));
		scene.SetInterpolation(interpolate);
		scene.OnRuntimeStart();
		ICharacterControl* characters = scene.GetService<ICharacterControl>();
		REQUIRE(characters);

		const glm::quat rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(glm::radians(-18.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		const glm::vec3 back = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
		const glm::dmat4 projection = glm::perspective(glm::radians(60.0), 3840.0 / 2160.0, 0.1, 1000.0);
		std::vector<ScreenSample> samples;
		glm::vec3 pivot(0.0f);
		bool started = false;
		const int frames = static_cast<int>(std::lround(2.0f / frame));
		for (int index = 0; index < frames; index++)
		{
			// Across the view: the camera looks along -X.
			REQUIRE(characters->MoveCharacter(walker, glm::vec3(0.0f, 0.0f, -6.5f)));
			scene.OnUpdateRuntime(Timestep(frame));
			const glm::vec3 feet(GetRenderedTransform(scene, walker)[3]);
			const glm::vec3 target = feet + glm::vec3(0.0f, 1.5f, 0.0f);
			pivot = started ? glm::mix(pivot, target, 1.0f - std::exp(-14.0f * frame)) : target;
			started = true;
			const glm::vec3 camera = pivot + back * 6.5f;
			const glm::dmat4 view = glm::inverse(glm::dmat4(glm::translate(glm::mat4(1.0f), camera) * glm::mat4_cast(rotation)));
			const glm::dvec4 clip = projection * view * glm::dvec4(glm::dvec3(feet + glm::vec3(0.0f, 1.0f, 0.0f)), 1.0);
			const glm::dvec2 ndc = glm::dvec2(clip) / clip.w;
			const double time = static_cast<double>(index + 1) * frame;
			if (time >= 0.5)
				samples.push_back(ScreenSample { time, glm::dvec2((ndc.x * 0.5 + 0.5) * 3840.0, (0.5 - ndc.y * 0.5) * 2160.0) });
		}
		// It ran the whole way.
		CHECK(walker.GetTransform().Translation.z < 8.0f);
		scene.OnRuntimeStop();
		return MeasureJitter(samples);
	}

	bool SameBytes(const void* a, const void* b, size_t size)
	{
		return std::memcmp(a, b, size) == 0;
	}

}

TEST_SUITE("Physics.Interpolation")
{
	TEST_CASE("A character walking at 6.5 m/s is drawn within a pixel of its path at every frame rate a following camera sees")
	{
		for (const float frame : { 1.0f / 1024.0f, 1.0f / 240.0f, 1.0f / 144.0f, 1.0f / 120.0f, 1.0f / 64.0f, 1.0f / 32.0f })
		{
			CAPTURE(1.0f / frame);
			const double jitter = MeasureCharacterJitter(true, frame);
			MESSAGE("Jitter at ", 1.0f / frame, " frames per second: ", jitter, " px");
			CHECK(jitter <= 1.0);
		}
		// Drawn as simulated, the same measure sees the owner's jitter above the step rate.
		const double stepped = MeasureCharacterJitter(false, 1.0f / 1024.0f);
		MESSAGE("Jitter without interpolation at 1024 frames per second: ", stepped, " px");
		CHECK(stepped >= 10.0);
	}

	TEST_CASE("Interpolation changes no simulated pose")
	{
		ScopedFixedMover moverScope;
		Scene interpolated("Interpolated");
		Scene plain("Plain");
		Scene* scenes[2] = { &interpolated, &plain };
		for (Scene* target : scenes)
		{
			Scene& scene = *target;
			scene.GetSettings().FixedTimestep = c_Step;
			CreateGround(scene);
			for (int index = 0; index < 64; index++)
			{
				CreateDynamicBox(scene, "Box", glm::vec3(static_cast<float>(index % 8) * 1.1f - 4.0f, 1.0f + static_cast<float>(index / 8) * 1.2f,
					static_cast<float>(index % 3) * 0.3f));
			}
			CreateWalker(scene, glm::vec3(-8.0f, 0.0f, 5.0f));
			// A kinematic body a mover pushes in OnFixedUpdate, through the boxes.
			Entity pusher = CreateDynamicBox(scene, "Pusher", glm::vec3(-10.0f, 0.5f, 0.0f));
			pusher.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			AddFixedMover(scene, pusher, glm::vec3(2.0f, 0.0f, 0.0f), 0.5f);
		}
		interpolated.SetInterpolation(true);
		for (Scene* scene : scenes)
			scene->OnRuntimeStart();

		std::mt19937 random(64);
		std::uniform_real_distribution<float> length(0.0005f, 0.04f);
		uint32_t blended = 0;
		for (int frame = 0; frame < 400; frame++)
		{
			const float timestep = length(random);
			for (Scene* scene : scenes)
			{
				ICharacterControl* characters = scene->GetService<ICharacterControl>();
				REQUIRE(characters);
				REQUIRE(characters->MoveCharacter(scene->FindEntityByName("Walker"), glm::vec3(3.0f, 0.0f, -1.0f)));
				scene->OnUpdateRuntime(Timestep(timestep));
			}
			// A renderer's update of the drawn transforms changes nothing either.
			interpolated.UpdateDrawnTransforms();
			blended += interpolated.GetInterpolationStats().Blended > 0 ? 1u : 0u;
			REQUIRE(interpolated.GetFixedStepCount() == plain.GetFixedStepCount());
			REQUIRE(interpolated.GetDroppedFixedStepCount() == plain.GetDroppedFixedStepCount());
			const entt::registry& left = interpolated.GetRegistry();
			const entt::registry& right = plain.GetRegistry();
			uint32_t different = 0;
			for (const auto [handle, transform, world] : left.view<TransformComponent, WorldTransformComponent>().each())
			{
				REQUIRE(right.valid(handle));
				if (!SameBytes(&transform, &right.get<TransformComponent>(handle), sizeof(TransformComponent))
					|| !SameBytes(&world.Matrix, &right.get<WorldTransformComponent>(handle).Matrix, sizeof(glm::mat4)))
					different++;
			}
			REQUIRE(different == 0);
		}
		CHECK(blended > 300);
		for (Scene* scene : scenes)
			scene->OnRuntimeStop();
	}

	TEST_CASE("Teleports draw no streak")
	{
		ScopedFixedMover moverScope;
		Scene scene("Teleports");
		scene.GetSettings().FixedTimestep = c_Step;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity rider = scene.CreateChildEntity(box, "Rider");
		rider.GetTransform().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		Entity walker = CreateWalker(scene, glm::vec3(5.0f, 0.0f, 0.0f));
		enum class Moment
		{
			Never,
			FixedUpdate,
			Update
		};
		Moment boxTeleport = Moment::Never;
		Moment walkerTeleport = Moment::Never;
		glm::vec3 boxTarget(0.0f);
		glm::vec3 walkerTarget(0.0f);
		const auto teleport = [&](Scene& updated, Moment now)
		{
			if (boxTeleport == now)
			{
				REQUIRE(updated.GetService<IBodyControl>()->Teleport(box, boxTarget, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
				boxTeleport = Moment::Never;
			}
			if (walkerTeleport == now)
			{
				REQUIRE(updated.GetService<ICharacterControl>()->TeleportCharacter(walker, walkerTarget));
				walkerTeleport = Moment::Never;
			}
		};
		FixedMoverSystem::FixedHook = [&](Scene& updated, float) { teleport(updated, Moment::FixedUpdate); };
		FixedMoverSystem::UpdateHook = [&](Scene& updated, Timestep) { teleport(updated, Moment::Update); };
		scene.SetInterpolation(true);
		scene.OnRuntimeStart();
		ICharacterControl* characters = scene.GetService<ICharacterControl>();
		REQUIRE(characters);
		REQUIRE(scene.GetService<IBodyControl>());

		// Both move: the box is thrown up, the walker walks.
		const auto frameOf = [&](float length)
		{
			REQUIRE(characters->MoveCharacter(walker, glm::vec3(4.0f, 0.0f, 0.0f)));
			scene.OnUpdateRuntime(Timestep(length));
		};
		scene.GetService<IBodyControl>()->SetLinearVelocity(box, glm::vec3(0.0f, 6.0f, 0.0f));
		for (int frame = 0; frame < 40; frame++)
			frameOf(1.0f / 1024.0f);

		for (const Moment moment : { Moment::FixedUpdate, Moment::Update })
		{
			CAPTURE(moment == Moment::FixedUpdate ? "in OnFixedUpdate" : "in OnUpdate");
			// 3 m away: within a step's reach at box3d's maximum speed, so that only the teleport's break draws it exactly.
			boxTarget = glm::vec3(box.GetTransform().Translation) + glm::vec3(3.0f, 0.0f, 0.0f);
			walkerTarget = glm::vec3(walker.GetTransform().Translation) + glm::vec3(0.0f, 0.0f, 3.0f);
			boxTeleport = moment;
			walkerTeleport = moment;
			// The frame that teleports them (OnUpdate runs in every frame, OnFixedUpdate in those that step).
			bool stepped = false;
			while (boxTeleport != Moment::Never)
			{
				const uint64_t steps = scene.GetFixedStepCount();
				frameOf(1.0f / 1024.0f);
				stepped = scene.GetFixedStepCount() != steps;
			}
			// A teleport in the steps, or before steps that did not come: drawn where they are on the frames after, until a step
			// moves them again. (Steps after a teleport in OnUpdate blend from where it put them.)
			if (moment == Moment::FixedUpdate || !stepped)
			{
				const uint64_t steps = scene.GetFixedStepCount();
				for (int frame = 0; frame < 64 && scene.GetFixedStepCount() == steps; frame++)
				{
					for (const Entity entity : { box, rider, walker })
						CHECK(SameMatrix(GetRenderedTransform(scene, entity), scene.GetWorldTransform(entity)));
					frameOf(1.0f / 1024.0f);
				}
			}
			// From then on the drawn poses lie near the targets, never back toward where they came from.
			for (int frame = 0; frame < 32; frame++)
			{
				CHECK(glm::length(glm::vec3(GetRenderedTransform(scene, box)[3]) - boxTarget) < 0.5f);
				CHECK(glm::length(glm::vec3(GetRenderedTransform(scene, walker)[3]) - walkerTarget) < 0.5f);
				frameOf(1.0f / 1024.0f);
			}
		}
		std::string error;
		CHECK_MESSAGE(scene.ValidateInterpolation(&error), error);
		scene.OnRuntimeStop();
	}
}
