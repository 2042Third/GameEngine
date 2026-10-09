// Entry points of a script module. strata_add_script_module() compiles this file into every module, so game code
// never defines them itself.

#include "StrataScript/StrataScript.h"

#if !defined(ST_SCRIPT_MODULE_NAME)
	#define ST_SCRIPT_MODULE_NAME "ScriptModule"
#endif

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_GetABIVersion(void)
{
	return ST_SCRIPT_ABI_VERSION;
}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_Load(const StrataScriptHostAPI* host, uint32_t hostABIVersion, StrataScriptModuleAPI* outModule)
{
	return Strata::Detail::LoadModule(host, hostABIVersion, outModule, ST_SCRIPT_MODULE_NAME);
}
