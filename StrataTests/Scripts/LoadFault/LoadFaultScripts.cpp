// A module that crashes while loading: the module constructs every class once to read its field defaults, and this
// class's constructor dereferences null. With STRATA_TEST_LOADFAULT_ABORT set, a static object calls abort() even
// earlier, while the library itself loads.

#include "StrataScript/StrataScript.h"

#include <cstdlib>

using namespace Strata;

namespace
{

	class AbortWhileLoading
	{
	public:
		AbortWhileLoading()
		{
			const char* value = std::getenv("STRATA_TEST_LOADFAULT_ABORT");
			if (value && *value)
				std::abort();
		}
	};

	const AbortWhileLoading s_AbortWhileLoading;

}

class CrashesWhileLoading : public Script
{
public:
	CrashesWhileLoading()
	{
		volatile int* pointer = nullptr;
		*pointer = 42;
	}
};

ST_SCRIPT_CLASS(CrashesWhileLoading) {}
