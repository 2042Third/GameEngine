// A shared library that a test script module links against (StrataTestScriptsDependent). It is built into its own
// directory, so loading the module only works when the engine finds it next to the module file.

#include <cstdint>

#if defined(_WIN32)
	#define ST_TEST_DEPENDENCY_EXPORT __declspec(dllexport)
#else
	#define ST_TEST_DEPENDENCY_EXPORT __attribute__((visibility("default")))
#endif

extern "C" ST_TEST_DEPENDENCY_EXPORT int32_t StrataTestScriptDependency_GetValue(void)
{
	return 42;
}
