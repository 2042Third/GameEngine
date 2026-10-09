// Entry points of a script module. strata_add_script_module() compiles this file into every module, so game code
// never defines them itself.

// Windows modules with their own (static) C runtime turn abort() into a crash the engine contains. With a shared C
// runtime (/MD) its abort handling belongs to every user of that runtime, so it is left alone (abort() then ends the
// process). On POSIX the engine reports SIGABRT and the process ends: there the C library also aborts on heap corruption
// while holding allocator locks, so an abort cannot be contained safely.
#if defined(_WIN32) && defined(_MSC_VER) && !defined(_DLL)
	#define ST_SCRIPT_CONTAIN_ABORT 1
#else
	#define ST_SCRIPT_CONTAIN_ABORT 0
#endif

#if ST_SCRIPT_CONTAIN_ABORT
	// This file's static objects are constructed in the C runtime's library segment, before the static objects of the
	// module's own code (user segment): abort() in a static initializer is contained too (the load then fails). Warning
	// C4073 only states that the initializers move to the library segment, which is the point.
	#pragma warning(push)
	#pragma warning(disable : 4073)
	#pragma init_seg(lib)
	#pragma warning(pop)
#endif

#include "StrataScript/StrataScript.h"

#if !defined(ST_SCRIPT_MODULE_NAME)
	#define ST_SCRIPT_MODULE_NAME "ScriptModule"
#endif

#if ST_SCRIPT_CONTAIN_ABORT
	#if !defined(WIN32_LEAN_AND_MEAN)
		#define WIN32_LEAN_AND_MEAN
	#endif
	#if !defined(NOMINMAX)
		#define NOMINMAX
	#endif
	#include <Windows.h>

	#include <csignal>
	#include <cstdlib>

namespace
{

	// The C runtime calls this from abort() (failed assert(), std::abort()) before it would end the process. The
	// exception unwinds to the engine's crash guard, which reports a crash of the current call.
	void __cdecl HandleAbort(int)
	{
		// The C runtime resets the handler before calling it; later aborts are handled the same way.
		std::signal(SIGABRT, &HandleAbort);
		RaiseException(ST_SCRIPT_ABORT_EXCEPTION_CODE, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}

	// Set up while the module's C runtime initializes, before any of the module's own static objects.
	class AbortContainment
	{
	public:
		AbortContainment()
		{
			// assert() reports to stderr instead of a message box that would block, and abort() writes no report of its
			// own (a dialog in Debug builds) before calling the handler.
			_set_error_mode(_OUT_TO_STDERR);
			_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			std::signal(SIGABRT, &HandleAbort);
		}
	};

	const AbortContainment s_AbortContainment;

}
#endif

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_GetABIVersion(void)
{
	return ST_SCRIPT_ABI_VERSION;
}

ST_SCRIPT_EXTERN_C ST_SCRIPT_EXPORT uint32_t StrataScript_Load(const StrataScriptHostAPI* host, uint32_t hostABIVersion, StrataScriptModuleAPI* outModule)
{
	return Strata::Detail::LoadModule(host, hostABIVersion, outModule, ST_SCRIPT_MODULE_NAME);
}
