#pragma once

#include "Strata/Core/Base.h"

#include <string>

namespace Strata
{

	struct CrashInfo
	{
		std::string Description; // Human-readable fault description
		uint64_t Code = 0;       // Windows exception code or POSIX signal number
		uint64_t Address = 0;    // Faulting instruction address, if known
	};

	// Contains hardware faults raised by untrusted native code (game scripts) so they cannot take down the
	// host process. Invoke() runs a function; if it raises an access violation, illegal instruction,
	// integer division by zero, stack overflow, or (on Windows) an unhandled C++ exception, execution
	// resumes at the Invoke() call, which returns false and describes the fault.
	//
	// Destructors of objects in the faulting call frames do not run, so the guarded code may leak.
	// Callers must treat the guarded module as unusable after a fault (e.g. stop play mode and unload it).
	// Guards may be nested and are per thread.
	class CrashGuard
	{
	public:
		using GuardedFunction = void (*)(void* userData);

		static bool Invoke(GuardedFunction function, void* userData, CrashInfo* outInfo = nullptr);
	};

}
