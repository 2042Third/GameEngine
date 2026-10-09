#pragma once

#include "Strata/Core/Base.h"

#include <string>

namespace Strata
{

	struct CrashInfo
	{
		std::string Description; // Human-readable fault description
		uint64_t Code = 0;       // Windows exception code or POSIX signal number (0 for a C++ exception on POSIX)
		uint64_t Address = 0;    // Faulting instruction address, if known
	};

	// Windows: the structured exception code with which guarded code reports that it called abort() (a failed assert(),
	// std::abort()). A C runtime would end the process instead; script modules built with the SDK install a SIGABRT
	// handler in their own runtime that raises this code (ST_SCRIPT_ABORT_EXCEPTION_CODE).
	constexpr uint32_t c_CrashGuardAbortExceptionCode = 0xE0535441u;

	// Contains hardware faults raised by untrusted native code (game scripts) so they cannot take down the
	// host process. Invoke() runs a function; if it raises an access violation, illegal instruction,
	// integer division by zero or stack overflow, or a C++ exception escapes it, execution resumes at the
	// Invoke() call, which returns false and describes the fault. On Windows that includes abort() reported through
	// c_CrashGuardAbortExceptionCode (the C runtime detects heap corruption by failing fast, never through abort()).
	// Uncontainable: fail-fast terminations (Windows __fastfail: /GS buffer overrun checks, invalid-parameter
	// failures of the C runtime, heap corruption the system detects); on POSIX abort() (SIGABRT), which the C library
	// also calls on heap corruption while it holds allocator locks - it is reported on stderr and the process ends the
	// way abort() ends it; and anything that ends the process directly (exit, _exit, TerminateProcess, SIGKILL).
	// A crash inside the C library's allocator (a corrupted heap) can leave the allocator locked: the guarded thread
	// then blocks at its next allocation.
	//
	// Destructors of objects in the faulting call frames do not run, so the guarded code may leak.
	// Callers must treat the guarded module as unusable after a fault (e.g. stop play mode and unload it).
	// Guards may be nested and are per thread.
	//
	// POSIX: the first Invoke installs process-wide handlers for the fault signals (and an alternate signal stack per
	// thread, on macOS with a small recovery stack: there the handler leaves by returning, the only way that makes the
	// kernel forget that the thread runs on its alternate stack). Signals no guard handles - raised outside guarded
	// calls, or sent by another process - go to the handlers installed before (called directly, so the guard keeps its
	// own), or take their default action. Code that replaces these handlers later (crash reporters, test frameworks
	// catching signals) must be set up first, or crashes are no longer contained. On macOS a previous handler that leaves
	// with longjmp instead of returning leaves the thread marked as running on the alternate stack, so stack overflows
	// on that thread can no longer be contained (the guard logs an error after its next contained fault). Thread
	// cancellation (glibc's forced unwinding) passes through guarded calls.
	class CrashGuard
	{
	public:
		using GuardedFunction = void (*)(void* userData);

		static bool Invoke(GuardedFunction function, void* userData, CrashInfo* outInfo = nullptr);
	};

}
