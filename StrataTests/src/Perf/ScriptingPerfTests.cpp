#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptSystem.h"

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
}
