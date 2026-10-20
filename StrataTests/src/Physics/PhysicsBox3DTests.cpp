#include "Strata/Core/Assert.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Physics/PhysicsTaskSystem.h"

#include "Physics/PhysicsTestUtils.h"
#include "TestHelpers.h"

#include <box3d/box3d.h>
#include <box3d/collision.h>
// box3d's own determinism scenario (Strata/vendor/box3d/shared, StrataTestsBox3DScenarios): human.h declares C functions
// without a C++ guard of its own.
extern "C"
{
#include "determinism.h"
}

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <atomic>
#include <cmath>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// box3d's golden values for its falling ragdolls on a mesh (test/test_determinism.c, single precision, four manifold
	// points): the step at which everything sleeps and the hash of the bodies' state then. Equal on every compiler and
	// instruction set only if box3d is compiled without floating-point contraction, and for any number of workers only if
	// the task system runs every task exactly once and finishTask waits for it.
	constexpr int c_RagdollSleepStep = 291;
	constexpr uint32_t c_RagdollHash = 0xFF4C605Bu;

	struct RagdollRun
	{
		int SleepStep = 0;
		uint32_t Hash = 0;
		int WorkerCount = 0;
		uint64_t Tasks = 0;
	};

	RagdollRun RunFallingRagdolls()
	{
		PhysicsRuntimeReference runtime;
		PhysicsTaskCounters counters;
		RagdollRun run;
		{
			PhysicsTaskSystem tasks(&counters);
			b3WorldDef definition = b3DefaultWorldDef();
			tasks.Configure(definition);
			run.WorkerCount = static_cast<int>(definition.workerCount);
			const b3WorldId world = b3CreateWorld(&definition);
			FallingRagdollData data = CreateFallingRagdolls(world);
			for (int step = 0; step < 500; step++)
			{
				b3World_Step(world, 1.0f / 60.0f, 4);
				if (UpdateFallingRagdolls(world, &data))
					break;
			}
			b3DestroyWorld(world);
			run.SleepStep = data.sleepStep;
			run.Hash = data.hash;
			DestroyFallingRagdolls(&data);
		}
		run.Tasks = counters.Tasks.load();
		return run;
	}

	// The points of a sphere of 48 segments and 24 rings: its hull has more faces and edges than box3d's hulls may have
	// at 128 vertices, so b3CreateHull refuses it there (and logs why).
	std::vector<b3Vec3> MakeSpherePoints()
	{
		std::vector<b3Vec3> points;
		constexpr int c_Segments = 48;
		constexpr int c_Rings = 24;
		for (int ring = 0; ring <= c_Rings; ring++)
		{
			const float phi = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(c_Rings);
			for (int segment = 0; segment < c_Segments; segment++)
			{
				const float theta = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(c_Segments);
				points.push_back(b3Vec3 { 0.5f * std::sin(phi) * std::cos(theta), 0.5f * std::cos(phi), 0.5f * std::sin(phi) * std::sin(theta) });
			}
		}
		return points;
	}

	std::atomic<uint32_t> s_ContinuedAssertions = 0;

	AssertAction ContinueOnAssert(const AssertInfo&)
	{
		s_ContinuedAssertions++;
		return AssertAction::Continue;
	}

}

TEST_SUITE("Physics.Box3D")
{
	TEST_CASE("box3d's cross-platform determinism scenario matches its golden hash, with any number of workers")
	{
		// Without a JobSystem the tasks run inline on the stepping thread.
		const RagdollRun single = RunFallingRagdolls();
		CHECK(single.WorkerCount == 1);
		CHECK(single.SleepStep == c_RagdollSleepStep);
		CHECK(single.Hash == c_RagdollHash);
		CHECK(single.Tasks > 0);

		// On Strata's workers: box3d splits each stage for the 4 workers and the stepping thread.
		ScopedJobSystem jobs(4);
		const RagdollRun parallel = RunFallingRagdolls();
		CHECK(parallel.WorkerCount == 5);
		CHECK(parallel.SleepStep == c_RagdollSleepStep);
		CHECK(parallel.Hash == c_RagdollHash);
		CHECK(parallel.Tasks > single.Tasks);
	}

	TEST_CASE("box3d's log messages reach Strata's log unless suppressed on their thread")
	{
		PhysicsRuntimeReference runtime;
		const std::vector<b3Vec3> points = MakeSpherePoints();

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		const uint64_t messages = PhysicsRuntime::GetLogMessageCount();
		b3HullData* refused = b3CreateHull(points.data(), static_cast<int>(points.size()), 128);
		CHECK(refused == nullptr);
		if (refused)
			b3DestroyHull(refused);
		CHECK(PhysicsRuntime::GetLogMessageCount() > messages);
		CHECK(CountLogMessages(logStart, "box3d: ") > 0);

		const uint64_t suppressedStart = Log::GetBuffer().GetLatestSequence();
		const uint64_t beforeSuppressed = PhysicsRuntime::GetLogMessageCount();
		{
			Box3DLogSuppression suppression;
			b3HullData* again = b3CreateHull(points.data(), static_cast<int>(points.size()), 128);
			CHECK(again == nullptr);
			if (again)
				b3DestroyHull(again);
		}
		CHECK(PhysicsRuntime::GetLogMessageCount() == beforeSuppressed);
		CHECK(CountLogMessages(suppressedStart, "box3d: ") == 0);

		// A vertex count whose hull fits box3d's face and edge limits builds the same points.
		b3HullData* reduced = b3CreateHull(points.data(), static_cast<int>(points.size()), 44);
		REQUIRE(reduced != nullptr);
		CHECK(reduced->vertexCount <= 44);
		b3DestroyHull(reduced);
	}

	TEST_CASE("box3d's asserts go through Strata's assert handling and are counted")
	{
		PhysicsRuntimeReference runtime;
		const b3WorldDef worldDefinition = b3DefaultWorldDef();
		const b3WorldId world = b3CreateWorld(&worldDefinition);
		const b3BodyDef bodyDefinition = b3DefaultBodyDef();
		const b3BodyId body = b3CreateBody(world, &bodyDefinition);

		// A hull of an unknown version: box3d asserts on it, and without asserts (Dist) refuses it all the same.
		b3BoxHull box = b3MakeBoxHull(0.5f, 0.5f, 0.5f);
		box.base.version = 0;
		const b3ShapeDef shapeDefinition = b3DefaultShapeDef();
		s_ContinuedAssertions = 0;
		const uint64_t assertions = PhysicsRuntime::GetAssertionCount();
		SetAssertHandler(&ContinueOnAssert);
		const b3ShapeId shape = b3CreateHullShape(body, &shapeDefinition, &box.base);
		SetAssertHandler(nullptr);
		CHECK(B3_IS_NULL(shape));
#if defined(ST_DIST)
		CHECK(PhysicsRuntime::GetAssertionCount() == assertions);
		CHECK(s_ContinuedAssertions == 0);
#else
		CHECK(PhysicsRuntime::GetAssertionCount() == assertions + 1);
		CHECK(s_ContinuedAssertions == 1);
#endif
		b3DestroyWorld(world);
	}
}
