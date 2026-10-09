# A game project's script build, as a game project sets it up: outside the engine build, through the StrataScriptCore
# package. Built by the StrataScriptCore.Package test (see BuildPackageProject.cmake).
cmake_minimum_required(VERSION 3.25)
project(PackageProjectScripts CXX)

set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata engine checkout")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${STRATA_ENGINE_DIR}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(PackageProjectScripts SOURCE_DIR Scripts)

# Test only: the package must compile scripts with the engine's glm definitions ('|'-separated, from the engine build).
set(STRATA_EXPECTED_GLM_DEFINITIONS "" CACHE STRING "glm definitions of the engine build (test only)")
if(STRATA_EXPECTED_GLM_DEFINITIONS)
	string(REPLACE "|" ";" expectedDefinitions "${STRATA_EXPECTED_GLM_DEFINITIONS}")
	get_target_property(packageDefinitions StrataScriptCore INTERFACE_COMPILE_DEFINITIONS)
	if(NOT packageDefinitions)
		set(packageDefinitions "")
	endif()
	list(SORT expectedDefinitions)
	list(SORT packageDefinitions)
	if(NOT expectedDefinitions STREQUAL packageDefinitions)
		message(FATAL_ERROR "The StrataScriptCore package defines '${packageDefinitions}' for glm, the engine '${expectedDefinitions}'")
	endif()
endif()
