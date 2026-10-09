// A module as if built against a newer SDK of the same ABI version: its StrataScriptModuleAPI has members appended after
// the ones this engine knows. It runs the SDK's loader, then hands the larger description to the engine the way the SDK
// does, so only the engine's struct size (StructSize on entry) may bound what is written.

#include "StrataScript/StrataScript.h"

#include <cstdint>

namespace
{

	struct NewerModuleAPI
	{
		StrataScriptModuleAPI Known;
		uint64_t Appended[8]; // Members a newer SDK added
	};

}

class Probe : public Strata::Script
{
public:
	int32_t Value = 7;
};

ST_SCRIPT_CLASS(Probe)
{
	ST_SCRIPT_FIELD(Value);
}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_GetABIVersion(void)
{
	return ST_SCRIPT_ABI_VERSION;
}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_Load(const StrataScriptHostAPI* host, uint32_t hostABIVersion, StrataScriptModuleAPI* outModule)
{
	if (!outModule)
		return StrataScriptResult_InvalidArgument;

	NewerModuleAPI newer = {};
	newer.Known.StructSize = sizeof(StrataScriptModuleAPI);
	const uint32_t result = Strata::Detail::LoadModule(host, hostABIVersion, &newer.Known, "NewerSDK");
	if (result != StrataScriptResult_Ok)
		return result;

	for (uint64_t& member : newer.Appended)
		member = UINT64_MAX;
	if (!Strata::Detail::WriteModuleAPI(outModule, &newer, sizeof(newer)))
	{
		newer.Known.Unload();
		return StrataScriptResult_ABIMismatch;
	}
	return StrataScriptResult_Ok;
}
