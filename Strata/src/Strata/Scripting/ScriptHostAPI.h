#pragma once

#include "StrataScript/ScriptABI.h"

namespace Strata
{

	class ScriptSystem;

	// The engine's implementation of the script host API (engine-internal). One table serves every module.
	const StrataScriptHostAPI& GetScriptHostAPI();

	// The context handed to scripts of a scene is its ScriptSystem. Host functions only accept registered contexts, on
	// the thread that registered them (the main thread). Registration is main-thread only.
	StrataScriptContext* RegisterScriptContext(ScriptSystem& system);
	void UnregisterScriptContext(ScriptSystem& system);

}
