# Stands in for a StrataTools.* test when CMake found no Python 3.10 or later at configure time: the test fails and says
# why, instead of missing silently. Usage: cmake -DTEST_NAME=<test> -P PythonMissing.cmake
message(FATAL_ERROR "${TEST_NAME} needs Python 3.10 or later on PATH when CMake configures")
