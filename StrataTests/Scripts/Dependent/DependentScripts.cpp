// A script module that imports a function from another shared library (StrataTestScriptDependency), for the tests of
// how the engine finds a module's dependencies.

#include "StrataScript/StrataScript.h"

#include <cstdint>

#if defined(_WIN32)
	#define ST_TEST_DEPENDENCY_IMPORT __declspec(dllimport)
#else
	#define ST_TEST_DEPENDENCY_IMPORT
#endif

extern "C" ST_TEST_DEPENDENCY_IMPORT int32_t StrataTestScriptDependency_GetValue(void);

// Value comes from the dependency, already when the module constructs the class to read its defaults.
class UsesDependency : public Strata::Script
{
public:
	int32_t Value = StrataTestScriptDependency_GetValue();
};

ST_SCRIPT_CLASS(UsesDependency)
{
	ST_SCRIPT_FIELD(Value);
}
