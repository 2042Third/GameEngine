// A hand-written script module that claims to be built against a newer script ABI. The engine must refuse it before
// calling StrataScript_Load.

#include "StrataScript/ScriptABI.h"

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_GetABIVersion(void)
{
	return ST_SCRIPT_ABI_VERSION + 1;
}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_Load(const StrataScriptHostAPI*, uint32_t, StrataScriptModuleAPI*)
{
	return StrataScriptResult_ABIMismatch;
}
