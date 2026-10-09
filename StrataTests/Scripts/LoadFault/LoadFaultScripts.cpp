// A module that crashes while loading: the module constructs every class once to read its field defaults, and this
// class's constructor dereferences null.

#include "StrataScript/StrataScript.h"

using namespace Strata;

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
