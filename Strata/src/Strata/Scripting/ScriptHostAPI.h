#pragma once

#include "StrataScript/ScriptABI.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace Strata
{

	class ScriptSystem;

	// The engine's implementation of the script host API (engine-internal). One table serves every module.
	const StrataScriptHostAPI& GetScriptHostAPI();

	// The context handed to scripts of a scene is its ScriptSystem. Host functions only accept registered contexts, on
	// the thread that registered them (the main thread). Registration is main-thread only.
	StrataScriptContext* RegisterScriptContext(ScriptSystem& system);
	void UnregisterScriptContext(ScriptSystem& system);

	struct ScriptHostFunctionCalls
	{
		std::string_view Name; // The member's name in StrataScriptHostAPI
		uint64_t Calls = 0;
	};

	// Diagnostics: every function of the host API table in declaration order, with the number of calls made through the
	// table (by any module, scene or thread; calls that were rejected count too) since the process started or the last
	// reset. The feature test uses it to prove that every host function is exercised. Thread-safe. Dist builds do not
	// count (host calls go straight to the implementation): there the list is empty and resetting does nothing.
	std::vector<ScriptHostFunctionCalls> GetScriptHostCallCounts();
	void ResetScriptHostCallCounts();

}
