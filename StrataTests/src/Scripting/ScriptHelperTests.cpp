#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Input/Input.h"

#include <string>

using namespace Strata;
using namespace Strata::Tests;

// The SDK's gameplay helpers (StrataScript/Gameplay.h) are checked inside the API test module
// (StrataTests/Scripts/API/HelperScripts.cpp): the engine has classes of the same names, so they cannot be compiled into
// the test executable.
TEST_SUITE("Scripting.Helpers")
{
	TEST_CASE("Random and Timer behave as documented")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Helpers");
		AddScriptEntry(entity, "HelperChecks");
		scene.OnRuntimeStart();
		CheckScriptChecks(GetScriptSystem(scene), entity, "HelperChecks", 25);
		scene.OnRuntimeStop();
	}

	TEST_CASE("KeyRepeat fires on the press, after the delay and then at the interval while the key is held")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity probe = scene.CreateEntity("Probe");
		AddScriptEntry(probe, "KeyRepeatProbe");
		scene.OnRuntimeStart();

		// Leaves the global input state clean for other tests, also when a check fails.
		struct ScopedInput
		{
			ScopedInput() { Input::Reset(); }
			~ScopedInput() { Input::Reset(); }
		} input;

		// 1/32 s frames add up exactly, so the delay (4 frames) and the interval (2 frames) end exactly on frames.
		for (int32_t frame = 0; frame < 20; frame++)
		{
			Input::BeginFrame();
			if (frame == 2 || frame == 15)
				Input::ProcessKey(Key::Space, true);
			else if (frame == 13 || frame == 16)
				Input::ProcessKey(Key::Space, false);
			scene.OnUpdateRuntime(1.0f / 32.0f);
		}
		CHECK(GetField<std::string>(GetScriptSystem(scene), probe, "KeyRepeatProbe", "Fires") == "2,6,8,10,12,15,");
		scene.OnRuntimeStop();
	}
}
