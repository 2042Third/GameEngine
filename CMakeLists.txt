cmake_minimum_required(VERSION 3.25)

project(Strata
	VERSION 0.1.0
	DESCRIPTION "Strata - a production-grade 3D game engine"
	HOMEPAGE_URL "https://github.com/2042Third/GameEngine"
	LANGUAGES C CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
	message(FATAL_ERROR "In-source builds are not supported. Configure with a preset, e.g. `cmake --preset windows`.")
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/Strata/vendor/glfw/CMakeLists.txt")
	message(FATAL_ERROR "Third-party submodules are missing. Run `git submodule update --init --recursive --depth 1`.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/CMake")

option(STRATA_BUILD_EDITOR "Build the Strata editor" ON)
option(STRATA_BUILD_RUNTIME "Build the Strata runtime (game player used for exported games)" ON)
# Defaults to OFF until the automation client lands (StrataCLI/).
option(STRATA_BUILD_CLI "Build StrataCLI (editor automation client and MCP server)" OFF)
option(STRATA_BUILD_TESTS "Build the Strata test suites" ON)
option(STRATA_ENABLE_TRACY "Instrument the engine for the Tracy profiler" OFF)
option(STRATA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in first-party targets" ON)

include(StrataConfigurations)
include(StrataCompilerOptions)
include(StrataShaders)

add_subdirectory(Strata/vendor)
add_subdirectory(StrataScriptCore)
add_subdirectory(Strata)

if(STRATA_BUILD_EDITOR)
	add_subdirectory(StrataEditor)
endif()

if(STRATA_BUILD_RUNTIME)
	add_subdirectory(StrataRuntime)
endif()

if(STRATA_BUILD_CLI)
	add_subdirectory(StrataCLI)
endif()

if(STRATA_BUILD_TESTS)
	enable_testing()
	add_subdirectory(StrataTests)
endif()
