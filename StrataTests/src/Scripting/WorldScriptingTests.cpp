#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/Scripting/ScriptSystem.h"
#include "Strata/World/WorldComponents.h"
#include "Strata/World/WorldStreamingSystem.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#include <chrono>
#include <string>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A frame the way a game runs it: the asset manager, then the scene with the frame's budget.
	void RunWorldFrame(MountedWorld& mounted, Scene& scene)
	{
		mounted.Manager->Update();
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f), &budget);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

}

TEST_SUITE("Scripting.World")
{
	TEST_CASE("Scripts prefetch a world's far cells, toggle its layers and let their requests go")
	{
		ScopedJobSystem jobs(2, 1);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		GeneratedWorldSpecification specification;
		specification.EntitiesPerCell = 6;
		specification.BodiesPerCell = 0;
		specification.PropsPerCell = 2;
		MountedWorld mounted("ScriptingWorld", specification);

		Scene scene("Persistent");
		scene.CreateEntity("World").AddComponent<WorldComponent>().World = mounted.World.World;
		scene.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		const Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, "WorldAPI");
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		REQUIRE(WaitUntil([&]()
		{
			RunWorldFrame(mounted, scene);
			return GetField<bool>(system, tester, "WorldAPI", "Done");
		}, std::chrono::seconds(60)));
		CHECK(GetField<int32_t>(system, tester, "WorldAPI", "Phase") == 5);
		CheckScriptChecks(system, tester, "WorldAPI", 14);

		// Released, the far corner goes: no source wants it.
		const WorldStreamingSystem* world = scene.GetSystem<WorldStreamingSystem>();
		REQUIRE(world);
		REQUIRE(world->GetStreamer());
		const WorldCellKey far { 1, 3, 3 };
		REQUIRE(WaitUntil([&]()
		{
			RunWorldFrame(mounted, scene);
			return world->GetStreamer()->GetCellStatus(far) == WorldCellStatus::Unloaded;
		}, std::chrono::seconds(60)));
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("In a scene that streams no world every world function fails, reported once each")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		const LogCapture log;
		Scene scene("Plain");
		const Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, "WorldWithoutWorld");
		scene.OnRuntimeStart();
		RunFrames(scene, 2);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "WorldWithoutWorld", "Done"));
		CheckScriptChecks(system, tester, "WorldWithoutWorld", 5);
		size_t reports = 0;
		for (const LogEntry& entry : log.GetEntries())
			reports += entry.Message.find("the scene streams no world") != std::string::npos ? 1 : 0;
		// One per function: RequestCellsAround twice, ReleaseCells, IsLoadedAround, SetLayerEnabled, IsLayerEnabled.
		CHECK(reports == 5);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}
}
