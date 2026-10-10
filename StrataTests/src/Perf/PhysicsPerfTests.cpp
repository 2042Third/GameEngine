#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	using Clock = std::chrono::steady_clock;

	constexpr float c_FrameTimestep = 1.0f / 60.0f;
	// ApplicationSpecification::MaxTimestep: the longest timestep an application gives a frame.
	constexpr float c_MaxTimestep = 0.25f;

	double MillisecondsSince(Clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	// Plays frames the way an application capped at 60 frames per second runs them, and returns the time they took in
	// milliseconds (their work, as Application::GetLastFrameWorkTime counts it: without the cap's waits, which are not
	// slept here). Each frame advances the scene by the time since the previous one began: at least a 60 Hz frame (the cap
	// waits out the rest), more after a slow frame, at most c_MaxTimestep. So a slow frame makes the next one simulate more,
	// as in a game.
	double PlayFramesAt60Hz(Scene& scene, uint32_t frames)
	{
		double work = 0.0;
		float timestep = c_FrameTimestep;
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			const Clock::time_point start = Clock::now();
			scene.OnUpdateRuntime(timestep);
			const double elapsed = MillisecondsSince(start);
			work += elapsed;
			timestep = std::clamp(static_cast<float>(elapsed / 1000.0), c_FrameTimestep, c_MaxTimestep);
		}
		return work;
	}

	// Plays box stacks (Perf::CreateBoxStacks) while the boxes land on each other and measures that busy phase: the median
	// 60 Hz frame (one fixed step each) against the budget of `<metric>.ActiveFrame`, and the median of Strata's part of a
	// step (synchronization, write-back, contacts: the step minus Jolt's own) against `<metric>.StrataStepTime`, which must
	// stay below Jolt's part.
	void MeasureFallingBoxes(size_t count, const std::string& metric)
	{
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		Perf::CreateBoxStacks(scene, count);
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.GetStats().DynamicBodyCount == count);

		constexpr uint32_t c_Warmup = 5;
		constexpr uint32_t c_Frames = 40;
		std::vector<double> frames;
		std::vector<double> joltSteps;
		std::vector<double> strataSteps;
		for (uint32_t frame = 0; frame < c_Warmup + c_Frames; frame++)
		{
			const Clock::time_point start = Clock::now();
			scene.OnUpdateRuntime(c_FrameTimestep);
			const double elapsed = MillisecondsSince(start);
			if (frame < c_Warmup)
				continue;
			// Read between the timed frames.
			const PhysicsStats stats = physics.GetStats();
			frames.push_back(elapsed);
			joltSteps.push_back(stats.JoltStepTime);
			strataSteps.push_back(static_cast<double>(stats.LastStepTime) - static_cast<double>(stats.JoltStepTime));
		}
		const Perf::Measurement frame = Perf::Summarize(frames);
		const Perf::Measurement jolt = Perf::Summarize(joltSteps);
		const Perf::Measurement strata = Perf::Summarize(strataSteps);
		MESSAGE(metric, ": frame median ", frame.MedianMs, " ms (p95 ", frame.P95Ms, " ms); step: Jolt ", jolt.MedianMs, " ms, Strata ", strata.MedianMs, " ms");

		// Still the busy phase: most boxes are awake.
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.ActiveBodyCount > count / 2);
		CHECK(stats.StepCount == c_Warmup + c_Frames);
		CHECK(stats.ContactOverflowSteps == 0);
		CHECK(CountLogMessages(logStart, "contacts were dropped") == 0);
		CHECK(strata.MedianMs < jolt.MedianMs);
		scene.OnRuntimeStop();

		Perf::CheckBudget(metric + ".ActiveFrame", frame.MedianMs);
		Perf::CheckBudget(metric + ".StrataStepTime", strata.MedianMs);
	}

}

TEST_SUITE("Perf.Physics")
{
	TEST_CASE("30,000 falling boxes start playing without a frame spiral")
	{
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		Perf::CreateBoxStacks(scene, 30'000);
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();

		// The first frames of a game capped at 60 frames per second that opens this scene: starting play (the world and its
		// bodies), then 30 frames, with the time dilation games run with (GameRuntimeOptions::TimeDilation).
		constexpr uint32_t c_Frames = 30;
		scene.SetTimeDilation(true);
		const Clock::time_point start = Clock::now();
		scene.OnRuntimeStart();
		const double startup = MillisecondsSince(start);
		const double elapsed = startup + PlayFramesAt60Hz(scene, c_Frames);
		const PhysicsStats stats = GetPhysics(scene).GetStats();
		MESSAGE("Started in ", startup, " ms and played ", c_Frames, " frames in ", elapsed - startup, " ms: ", stats.StepCount, " steps, ",
			stats.DroppedSteps, " dropped");
		CHECK(stats.StepCount >= c_Frames);

		const size_t droppedContacts = CountLogMessages(logStart, "contacts were dropped");
		CHECK(droppedContacts == 0);
		scene.OnRuntimeStop();

		Perf::CheckBudget("Perf.Physics.Boxes30k.StartAnd30Frames", elapsed);
		Perf::CheckBudget("Perf.Physics.Boxes30k.DroppedContactWarnings", static_cast<double>(droppedContacts));
	}

	TEST_CASE("70,000 boxes all get a body and step within budget")
	{
		{
			Perf::ScopedApplicationJobSystem jobs;
			Scene scene;
			Perf::CreateBoxStacks(scene, 70'000);
			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			scene.OnRuntimeStart();
			const PhysicsStats stats = GetPhysics(scene).GetStats();
			const size_t bodyErrors = CountLogMessages(logStart, "cannot create a body");
			CHECK(stats.BodyCount == 70'001);
			CHECK(stats.PendingBodyCount == 0);
			CHECK(bodyErrors == 0);
			scene.OnRuntimeStop();
			Perf::CheckBudget("Perf.Physics.Boxes70k.BodyErrors", static_cast<double>(bodyErrors));
		}
		MeasureFallingBoxes(70'000, "Perf.Physics.Boxes70k");
	}

	TEST_CASE("10,000 falling boxes")
	{
		MeasureFallingBoxes(10'000, "Perf.Physics.Boxes10k");
	}

	TEST_CASE("A world at the largest capacities a scene may ask for reserves less than 1 GiB")
	{
		// What playing a scene reserves for its world: the private memory (on Windows, the commit charge: also memory not
		// touched yet) the process gained from the start of play to its second step.
		constexpr uint64_t c_GiB = 1024ull * 1024 * 1024;
		constexpr double c_MB = 1000.0 * 1000.0; // The unit of the perf lab's memory budgets
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		CreateGround(scene);
		CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f));
		const auto measureReserved = [&](const PhysicsCapacitySettings& capacity) -> uint64_t
		{
			scene.GetSettings().PhysicsCapacity = capacity;
			const std::optional<ProcessMemoryInfo> before = Platform::GetProcessMemory();
			REQUIRE(before);
			scene.OnRuntimeStart();
			StepScene(scene, 2);
			const std::optional<ProcessMemoryInfo> during = Platform::GetProcessMemory();
			REQUIRE(during);
			CHECK(GetPhysics(scene).GetStats().MaxBodies == capacity.MaxBodies);
			scene.OnRuntimeStop();
			return during->PrivateBytes > before->PrivateBytes ? during->PrivateBytes - before->PrivateBytes : 0;
		};

		const PhysicsCapacitySettings largest { PhysicsCapacitySettings::c_MaxBodiesLimit, PhysicsCapacitySettings::c_MaxBodyPairsLimit,
			PhysicsCapacitySettings::c_MaxContactConstraintsLimit };
		const uint64_t largestReserved = measureReserved(largest);
		// The most bodies, with the pairs and contacts sized automatically for them.
		const uint64_t mostBodiesReserved = measureReserved({ PhysicsCapacitySettings::c_MaxBodiesLimit, 0, 0 });
		MESSAGE("Reserved for the largest capacities: ", static_cast<double>(largestReserved) / c_MB, " MB; for the most bodies: ",
			static_cast<double>(mostBodiesReserved) / c_MB, " MB");
		CHECK(largestReserved < c_GiB);
		CHECK(mostBodiesReserved < c_GiB);
		Perf::CheckBudget("Perf.Physics.LargestCapacities.ReservedMemory", static_cast<double>(largestReserved) / c_MB);
	}

	TEST_CASE("Creating 10,000 bodies in one frame")
	{
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation = glm::vec3(75.0f, -0.5f, 75.0f);
		floor.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(100.0f, 0.5f, 100.0f);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		scene.OnUpdateRuntime(c_FrameTimestep);

		// A spawn of 10,000 boxes (a prefab burst, a streamed cell): the entities exist, the next physics call builds their
		// bodies on the main thread. Only that is timed; the boxes go again before the next round.
		constexpr uint32_t c_Warmup = 2;
		constexpr uint32_t c_Rounds = 8;
		std::vector<double> samples;
		for (uint32_t round = 0; round < c_Warmup + c_Rounds; round++)
		{
			std::vector<Entity> boxes;
			boxes.reserve(10'000);
			for (uint32_t index = 0; index < 10'000; index++)
			{
				Entity box = scene.CreateEntity("Spawned");
				box.GetTransform().Translation = glm::vec3(static_cast<float>(index % 100) * 1.5f, 5.0f + static_cast<float>(round) * 2.0f, static_cast<float>(index / 100) * 1.5f);
				box.AddComponent<RigidBodyComponent>();
				box.AddComponent<BoxColliderComponent>();
				boxes.push_back(box);
			}

			const Clock::time_point start = Clock::now();
			const bool built = physics.HasBody(boxes.back());
			const double elapsed = MillisecondsSince(start);
			CHECK(built);
			if (round >= c_Warmup)
				samples.push_back(elapsed);

			scene.DestroyEntities(boxes);
			scene.OnUpdateRuntime(c_FrameTimestep);
			CHECK(physics.GetStats().DynamicBodyCount == 0);
		}
		scene.OnRuntimeStop();

		const Perf::Measurement creation = Perf::Summarize(samples);
		MESSAGE("Creating 10,000 bodies: median ", creation.MedianMs, " ms, max ", creation.MaxMs, " ms");
		Perf::CheckBudget("Perf.Physics.Create10kBodies", creation.MedianMs);
	}
}
