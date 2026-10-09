// Minimal shared library loaded by the DynamicLibrary tests.

#if defined(_WIN32)
	#define ST_TEST_LIBRARY_EXPORT extern "C" __declspec(dllexport)
#else
	#define ST_TEST_LIBRARY_EXPORT extern "C" __attribute__((visibility("default")))
#endif

ST_TEST_LIBRARY_EXPORT int StrataTestLibrary_Add(int a, int b)
{
	return a + b;
}

ST_TEST_LIBRARY_EXPORT const char* StrataTestLibrary_GetName()
{
	return "StrataTestLibrary";
}
