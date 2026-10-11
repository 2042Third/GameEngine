#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

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

	// A prefab of `count` entities nested ten to a parent (Perf::CreateNestedHierarchy), each drawing the built-in cube with
	// a tag, as props and enemies do.
	Ref<Prefab> CreateMeshPrefab(size_t count)
	{
		Scene authoring("Authoring");
		const Perf::NestedHierarchy hierarchy = Perf::CreateNestedHierarchy(authoring, count);
		for (Entity entity : hierarchy.Entities)
		{
			entity.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
			entity.AddComponent<TagComponent>().Tag = "Prop";
		}
		Ref<Prefab> prefab = Prefab::CreateFromEntities(authoring, { hierarchy.Root });
		REQUIRE(prefab);
		REQUIRE(prefab->GetEntityCount() == count);
		prefab->Handle = UUID();
		return prefab;
	}

	// The median time of `samples` calls of `create` (after `warmup` untimed ones), each followed by `destroy`, untimed, so that
	// every call instantiates into a scene of the same size.
	template<typename Create, typename Destroy>
	double MeasureCreation(std::string_view name, uint32_t warmup, uint32_t samples, Create&& create, Destroy&& destroy)
	{
		std::vector<double> times;
		times.reserve(samples);
		for (uint32_t call = 0; call < warmup + samples; call++)
		{
			const auto start = std::chrono::steady_clock::now();
			create();
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			destroy();
			if (call >= warmup)
				times.push_back(milliseconds);
		}
		const Perf::Measurement measurement = Perf::Summarize(times);
		MESSAGE(name, ": median ", measurement.MedianMs, " ms, p95 ", measurement.P95Ms, " ms, max ", measurement.MaxMs, " ms");
		return measurement.MedianMs;
	}

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

	TEST_CASE("A million-entity scene is copied for play storage by storage")
	{
		Perf::ScopedApplicationJobSystem jobs;
		Ref<Scene> scene = CreateRef<Scene>("Edited");
		Perf::CreateNestedHierarchy(*scene, 1'000'000);
		scene->UpdateWorldTransforms();

		// What starting play mode copies (EditorContext::StartRuntime): each copy timed, its destruction not.
		Ref<Scene> copy;
		Perf::CheckBudgetConfirmed("Perf.Scene.Copy1M", [&]
		{
			return MeasureCreation("Perf.Scene.Copy1M", 1, 7, [&] { copy = Scene::Copy(scene); }, [&] { copy.reset(); });
		});
		copy = Scene::Copy(scene);
		CHECK(copy->GetEntityCount() == scene->GetEntityCount());
		CHECK(copy->GetTransformUpdateCount() == 0);
		copy->UpdateWorldTransforms();
		CHECK(copy->GetTransformUpdateCount() == 0);
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

	TEST_CASE("Prefab instances are copied from the prefab's compiled entities")
	{
		Perf::ScopedApplicationJobSystem jobs;
		// A scene that already holds entities, as a running level does.
		Scene scene("Level");
		Perf::CreateNestedHierarchy(scene, 10'000);
		scene.UpdateWorldTransforms();

		// One instance of a small and of a large prefab: the median call of many.
		const Ref<Prefab> small = CreateMeshPrefab(10);
		const Ref<Prefab> large = CreateMeshPrefab(1000);
		std::vector<Entity> roots;
		const auto destroyRoots = [&]
		{
			scene.DestroyEntities(roots);
			roots.clear();
		};
		Perf::CheckBudgetConfirmed("Perf.Scene.InstantiatePrefab10", [&]
		{
			// Microseconds: batches of 100 instances (destroyed together afterwards), the time per instance.
			constexpr uint32_t c_Batch = 100;
			const double batchMs = MeasureCreation("Perf.Scene.InstantiatePrefab10 (100 instances)", 5, 101, [&]
			{
				for (uint32_t index = 0; index < c_Batch; index++)
					roots.push_back(small->Instantiate(scene).front());
			}, destroyRoots);
			return batchMs * 1000.0 / c_Batch;
		});
		Perf::CheckBudgetConfirmed("Perf.Scene.InstantiatePrefab1k", [&]
		{
			return MeasureCreation("Perf.Scene.InstantiatePrefab1k", 5, 101, [&] { roots = large->Instantiate(scene); }, destroyRoots);
		});

		// A thousand instances of the small prefab in one call, each placed by its transform.
		std::vector<glm::mat4> transforms;
		for (uint32_t index = 0; index < 1000; index++)
			transforms.push_back(glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(index % 32) * 3.0f, 0.0f, static_cast<float>(index / 32) * 3.0f)));
		Perf::CheckBudgetConfirmed("Perf.Scene.InstantiateMany1000x10", [&]
		{
			return MeasureCreation("Perf.Scene.InstantiateMany1000x10", 3, 41, [&]
			{
				roots = small->InstantiateMany(scene, transforms);
				REQUIRE(roots.size() == 1000);
			}, destroyRoots);
		});
		CHECK(scene.GetEntityCount() == 10'000);
	}
}
