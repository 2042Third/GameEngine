// Scripts that crash on purpose, for the crash containment tests.

// assert() stays active in every configuration: the "Assert" fault needs it.
#undef NDEBUG
#include <cassert>

#include "StrataScript/StrataScript.h"

#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

using namespace Strata;

namespace
{

	int Recurse(int depth)
	{
		// The base case is unreachable in practice; it only keeps compilers from flagging infinite recursion. The
		// addition after the call prevents tail-call optimization from turning this into a loop.
		volatile int currentDepth = depth;
		volatile char padding[1024];
		padding[0] = static_cast<char>(currentDepth);
		if (currentDepth == INT_MAX)
			return 0;
		return Recurse(currentDepth + 1) + padding[0];
	}

	void Throw()
	{
		throw std::runtime_error("This exception leaves a noexcept function on purpose");
	}

	// An exception leaving a noexcept function calls std::terminate.
	void ThrowThroughNoexcept() noexcept
	{
		Throw();
	}

	void Crash(const std::string& kind)
	{
		if (kind == "NullDereference")
		{
			volatile int* pointer = nullptr;
			*pointer = 42;
		}
		else if (kind == "DivideByZero")
		{
			volatile int divisor = 0;
			volatile int result = 100 / divisor;
			(void)result;
		}
		else if (kind == "StackOverflow")
		{
			volatile int result = Recurse(0);
			(void)result;
		}
		else if (kind == "Abort")
		{
			std::abort();
		}
		else if (kind == "Assert")
		{
			volatile bool holds = false;
			assert(holds && "This assertion fails on purpose");
		}
		else if (kind == "Terminate")
		{
			std::terminate();
		}
		else if (kind == "TerminateFromNoexcept")
		{
			ThrowThroughNoexcept();
		}
	}

}

// Crashes with Fault ("NullDereference", "DivideByZero", "StackOverflow", "Abort", "Assert", "Terminate" or
// "TerminateFromNoexcept") in the callback named by FaultIn.
class Faulty : public Script
{
public:
	std::string Fault;
	std::string FaultIn = "OnUpdate";

	void OnCreate() override { CrashIn("OnCreate"); }
	void OnUpdate(float) override { CrashIn("OnUpdate"); }
	void OnFixedUpdate(float) override { CrashIn("OnFixedUpdate"); }
	void OnLateUpdate(float) override { CrashIn("OnLateUpdate"); }
	void OnDestroy() override { CrashIn("OnDestroy"); }
private:
	void CrashIn(const char* callback)
	{
		if (FaultIn == callback)
			Crash(Fault);
	}
};

ST_SCRIPT_CLASS(Faulty)
{
	ST_SCRIPT_FIELD(Fault);
	ST_SCRIPT_FIELD(FaultIn);
}

class Healthy : public Script
{
public:
	int32_t Updates = 0;
	int32_t Creates = 0;

	void OnCreate() override { Creates++; }
	void OnUpdate(float) override { Updates++; }
};

ST_SCRIPT_CLASS(Healthy)
{
	ST_SCRIPT_FIELD(Updates);
	ST_SCRIPT_FIELD(Creates);
}

// Crashes while being constructed for an entity (not for the instance the module builds to read defaults).
class FaultInConstructor : public Script
{
public:
	FaultInConstructor()
	{
		if (GetEntity().GetID() != 0)
			Crash("NullDereference");
	}
};

ST_SCRIPT_CLASS(FaultInConstructor) {}

// Adds FaultInConstructor to its entity: the crash happens in a call nested inside this script's OnUpdate.
class NestedFault : public Script
{
public:
	bool ContinuedAfterFault = false;

	void OnUpdate(float) override
	{
		GetEntity().AddScript("FaultInConstructor");
		ContinuedAfterFault = true;
	}
};

ST_SCRIPT_CLASS(NestedFault)
{
	ST_SCRIPT_FIELD(ContinuedAfterFault);
}

// Busy-waits for Milliseconds in every update (for the watchdog).
class Slow : public Script
{
public:
	int32_t Milliseconds = 0;

	void OnUpdate(float) override
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(Milliseconds);
		while (std::chrono::steady_clock::now() < end)
		{
		}
	}
};

ST_SCRIPT_CLASS(Slow)
{
	ST_SCRIPT_FIELD(Milliseconds);
}
