# Strata automated tests (doctest).
#
# Test suites whose names start with "GPU" need a Vulkan-capable device; they are registered as a
# separate CTest test labelled "gpu" so machines without a GPU can run `ctest -LE gpu`.

# Small shared library used by the DynamicLibrary tests.
add_library(StrataTestLibrary SHARED "${CMAKE_CURRENT_SOURCE_DIR}/TestLibrary/TestLibrary.cpp")
strata_configure_target(StrataTestLibrary)
set_target_properties(StrataTestLibrary PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

file(GLOB_RECURSE STRATA_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StrataTests ${STRATA_TEST_SOURCES})
strata_configure_target(StrataTests)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_TEST_SOURCES})
target_link_libraries(StrataTests PRIVATE Strata doctest)
target_include_directories(StrataTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(StrataTests PRIVATE
	"STRATA_TEST_LIBRARY_NAME=\"$<TARGET_FILE_NAME:StrataTestLibrary>\""
	"STRATA_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
add_dependencies(StrataTests StrataTestLibrary)

# The CLI tests (src/CLI) compile only when StrataCLI is part of the build.
if(TARGET StrataCLILib)
	target_link_libraries(StrataTests PRIVATE StrataCLILib)
	target_compile_definitions(StrataTests PRIVATE STRATA_TESTS_HAVE_CLI)
endif()

if(WIN32)
	target_sources(StrataTests PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
endif()

add_test(NAME StrataTests.Core
	COMMAND StrataTests --test-suite-exclude=GPU* --no-intro=true
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")

add_test(NAME StrataTests.GPU
	COMMAND StrataTests --test-suite=GPU* --no-intro=true
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")
set_tests_properties(StrataTests.GPU PROPERTIES LABELS "gpu")
