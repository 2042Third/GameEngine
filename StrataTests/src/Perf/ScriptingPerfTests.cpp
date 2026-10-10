#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptSystem.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Plays `count` entities whose script has a trivial OnUpdate and measures the play frames once the scene runs steadily:
	// the frame time against `metric`'s budget, and the update order, which must not be recomputed on frames without changes.
	void MeasureTrivialScripts(size_t count, const std::string& metric)
	{
		Perf::ScopedApplicationJobSystem jobs;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const std::vector<Entity> entities = Perf::CreateScriptedEntities(scene, count);
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		REQUIRE(system.GetInstanceCount() == count);

		constexpr uint32_t c_Warmup = 5;
		constexpr uint32_t c_Frames = 60;
		RunFrames(scene, 1);
		const uint64_t rebuilds = system.GetUpdateOrderRebuildCount();
		const Perf::Measurement frame = Perf::Measure(metric, c_Warmup, c_Frames, [&]
		{
			scene.OnUpdateRuntime(1.0f / 60.0f);
		});
		const uint64_t frameRebuilds = system.GetUpdateOrderRebuildCount() - rebuilds;
		CHECK(frameRebuilds == 0);
		// Every script ran in every frame.
		CHECK(GetField<int32_t>(system, entities.front(), Perf::c_TrivialScriptClass, "Updates") == static_cast<int32_t>(1 + c_Warmup + c_Frames));
		CHECK(GetField<int32_t>(system, entities.back(), Perf::c_TrivialScriptClass, "Updates") == static_cast<int32_t>(1 + c_Warmup + c_Frames));
		scene.OnRuntimeStop();

		Perf::CheckBudget(metric, frame.MedianMs);
		Perf::CheckBudget(metric + ".OrderRebuilds", static_cast<double>(frameRebuilds));
	}

}

TEST_SUITE("Perf.Scripting")
{
	TEST_CASE("100,000 scripts with a trivial OnUpdate")
	{
		MeasureTrivialScripts(100'000, "Perf.Scripting.TrivialUpdate100k");
	}

	TEST_CASE("300,000 scripts with a trivial OnUpdate")
	{
		MeasureTrivialScripts(300'000, "Perf.Scripting.TrivialUpdate300k");
	}

	TEST_CASE("A play frame in which scripts destroy 10,000 of 100,000 sibling roots")
	{
		// Every tenth root destroys itself in its first update; the scene destroys them all at the end of the frame, compacting
		// the root list once. Each round builds a fresh scene and times its first frame.
		Perf::ScopedApplicationJobSystem jobs;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		constexpr int c_Rounds = 3;
		std::vector<double> samples;
		for (int round = 0; round < c_Rounds; round++)
		{
			Scene scene;
			// OnDestroy removes a script from this entity, which has none: nothing happens.
			scene.CreateEntity("Target");
			for (size_t index = 0; index < 100'000; index++)
			{
				if (index % 10 != 0)
				{
					scene.CreateEntity("Root");
					continue;
				}
				Entity scripted = Perf::CreateScriptedEntities(scene, 1, "RemoveOnDestroy").front();
				ScriptEntry& entry = scripted.GetComponent<ScriptComponent>().Scripts.front();
				AddFieldOverride(entry, "DestroySelf", PropertyType::Bool, true);
				AddFieldOverride(entry, "Target", PropertyType::String, std::string("Target"));
				AddFieldOverride(entry, "ClassName", PropertyType::String, std::string("Missing"));
			}
			scene.OnRuntimeStart();
			REQUIRE(GetScriptSystem(scene).GetInstanceCount() == 10'000);

			const auto start = std::chrono::steady_clock::now();
			scene.OnUpdateRuntime(1.0f / 60.0f);
			samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			CHECK(scene.GetEntityCount() == 90'001);
			CHECK(scene.GetRootEntities().size() == 90'001);
			CHECK(GetScriptSystem(scene).GetInstanceCount() == 0);
			scene.OnRuntimeStop();
		}
		const Perf::Measurement frame = Perf::Summarize(samples);
		MESSAGE("Perf.Scripting.ScriptedDestroy10kOf100kRoots: median ", frame.MedianMs, " ms");
		Perf::CheckBudget("Perf.Scripting.ScriptedDestroy10kOf100kRoots", frame.MedianMs);
	}
}
