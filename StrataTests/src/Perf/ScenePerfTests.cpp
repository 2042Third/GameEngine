#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <cstdint>
#include <string>

// Statistics: every time is the median of its samples (batches of calls where one call is too short to time), which the
// machine's other work moves only when it slows down most of the measurement; a measurement it slowed down that much is
// confirmed by measuring again (Perf::CheckBudgetsConfirmed). The counts are exact.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Calls that take nanoseconds are timed in batches, since one call is below the clock's resolution (about 0.1 us): a
	// batch spans hundreds of clock ticks or more.
	constexpr uint32_t c_CallsPerBatch = 1000;
	constexpr uint32_t c_CleanUpdatesPerBatch = 100'000;

	// Moves an entity back and forth along x (so repeated moves stay near the start) and signals the write.
	void Nudge(Entity entity, uint32_t iteration)
	{
		entity.GetTransform().Translation.x += iteration % 2 == 0 ? 0.5f : -0.5f;
		entity.MarkModified<TransformComponent>();
	}

}

TEST_SUITE("Perf.Scene")
{
	TEST_CASE("A million nested entities cost nothing per frame while nothing changes")
	{
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		const Perf::NestedHierarchy hierarchy = Perf::CreateNestedHierarchy(scene, 1'000'000);
		scene.UpdateWorldTransforms();
		REQUIRE(scene.GetTransformUpdateCount() == 1'000'000);

		// A clean update (every edit frame does one; play frames do two) only finds that nothing changed.
		Perf::CheckBudgetsConfirmed({ "Perf.Scene.CleanTransformUpdate1M", "Perf.Scene.CleanTransformUpdate1M.Recomputed" }, [&]
		{
			const uint64_t updated = scene.GetTransformUpdateCount();
			const Perf::Measurement clean = Perf::Measure("Perf.Scene.CleanTransformUpdate1M (100,000 calls)", 2, 50, [&]
			{
				for (uint32_t call = 0; call < c_CleanUpdatesPerBatch; call++)
					scene.UpdateWorldTransforms();
			});
			const uint64_t recomputed = scene.GetTransformUpdateCount() - updated;
			CHECK(recomputed == 0);
			return Perf::MetricValues {
				{ "Perf.Scene.CleanTransformUpdate1M", clean.MedianMs * 1000.0 / c_CleanUpdatesPerBatch },
				{ "Perf.Scene.CleanTransformUpdate1M.Recomputed", static_cast<double>(recomputed) } };
		});

		// Moving the root recomputes everything, on the job system.
		uint32_t iteration = 0;
		Perf::CheckBudgetConfirmed("Perf.Scene.MoveRoot1M", [&]
		{
			constexpr uint32_t c_Warmup = 2;
			constexpr uint32_t c_Moves = 21;
			const uint64_t beforeMoves = scene.GetTransformUpdateCount();
			const Perf::Measurement moveRoot = Perf::Measure("Perf.Scene.MoveRoot1M", c_Warmup, c_Moves, [&]
			{
				Nudge(hierarchy.Root, iteration++);
				scene.UpdateWorldTransforms();
			});
			CHECK(scene.GetTransformUpdateCount() - beforeMoves == uint64_t(c_Warmup + c_Moves) * 1'000'000);
			return moveRoot.MedianMs;
		});

		// A play frame of the idle scene: the built-in systems run, the transforms stay clean.
		scene.OnRuntimeStart();
		Perf::CheckBudgetConfirmed("Perf.Scene.IdlePlayFrame1M", [&]
		{
			const uint64_t beforePlay = scene.GetTransformUpdateCount();
			const Perf::Measurement playFrames = Perf::Measure("Perf.Scene.IdlePlayFrame1M (1000 frames)", 2, 50, [&]
			{
				for (uint32_t frame = 0; frame < c_CallsPerBatch; frame++)
					scene.OnUpdateRuntime(1.0f / 60.0f);
			});
			CHECK(scene.GetTransformUpdateCount() == beforePlay);
			return playFrames.MedianMs * 1000.0 / c_CallsPerBatch;
		});
		scene.OnRuntimeStop();
	}

	TEST_CASE("Moving one entity of a 100,000-entity hierarchy recomputes only what it moved")
	{
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene;
		const Perf::NestedHierarchy hierarchy = Perf::CreateNestedHierarchy(scene, 100'000);
		scene.UpdateWorldTransforms();

		// A leaf of the deepest level: exactly one world transform per move.
		const Entity leaf = hierarchy.Entities.back();
		REQUIRE(leaf.GetChildren().empty());
		REQUIRE(scene.GetDepth(leaf) == 5);
		uint32_t iteration = 0;
		Perf::CheckBudgetsConfirmed({ "Perf.Scene.MoveLeaf100k", "Perf.Scene.MoveLeaf100k.Recomputed" }, [&]
		{
			constexpr uint32_t c_LeafWarmup = 2;
			constexpr uint32_t c_LeafBatches = 100;
			const uint64_t before = scene.GetTransformUpdateCount();
			const Perf::Measurement moveLeaf = Perf::Measure("Perf.Scene.MoveLeaf100k (1000 moves)", c_LeafWarmup, c_LeafBatches, [&]
			{
				for (uint32_t move = 0; move < c_CallsPerBatch; move++)
				{
					Nudge(leaf, iteration++);
					scene.UpdateWorldTransforms();
				}
			});
			const uint64_t leafMoves = uint64_t(c_LeafWarmup + c_LeafBatches) * c_CallsPerBatch;
			const uint64_t leafRecomputed = scene.GetTransformUpdateCount() - before;
			CHECK(leafRecomputed == leafMoves);
			return Perf::MetricValues {
				{ "Perf.Scene.MoveLeaf100k", moveLeaf.MedianMs * 1000.0 / c_CallsPerBatch },
				{ "Perf.Scene.MoveLeaf100k.Recomputed", static_cast<double>(leafRecomputed) / static_cast<double>(leafMoves) } };
		});

		// The root: the whole hierarchy. A move takes a third of a millisecond, so many of them span enough time for the
		// median to ignore what the machine's other work does to a few.
		Perf::CheckBudgetConfirmed("Perf.Scene.MoveRoot100k", [&]
		{
			constexpr uint32_t c_RootWarmup = 5;
			constexpr uint32_t c_RootMoves = 101;
			const uint64_t before = scene.GetTransformUpdateCount();
			const Perf::Measurement moveRoot = Perf::Measure("Perf.Scene.MoveRoot100k", c_RootWarmup, c_RootMoves, [&]
			{
				Nudge(hierarchy.Root, iteration++);
				scene.UpdateWorldTransforms();
			});
			CHECK(scene.GetTransformUpdateCount() - before == uint64_t(c_RootWarmup + c_RootMoves) * 100'000);
			return moveRoot.MedianMs;
		});

		std::string error;
		CHECK_MESSAGE(scene.ValidateWorldTransforms(&error), error);
	}
}
