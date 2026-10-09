# StrataScriptCore package for game projects: builds the project's scripts into a module without building the engine.
#
#   cmake_minimum_required(VERSION 3.25)
#   project(MyGameScripts CXX)
#   find_package(StrataScriptCore CONFIG REQUIRED PATHS "<engine>/StrataScriptCore/CMake" NO_DEFAULT_PATH)
#   strata_add_script_module(MyGameScripts SOURCE_DIR Scripts)

include_guard(GLOBAL)

get_filename_component(_strataScriptCoreRoot "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT TARGET StrataScriptCore)
	if(NOT EXISTS "${_strataScriptCoreRoot}/../Strata/vendor/glm/glm/glm.hpp")
		message(FATAL_ERROR "StrataScriptCore: glm not found in the engine's vendor directory; run `git submodule update --init --recursive --depth 1` in the engine")
	endif()

	# The glm definitions must match the engine's glm target (Strata/vendor/CMakeLists.txt), so scripts and the engine
	# agree on glm's behavior; the StrataScriptCore.Package test compares them.
	add_library(StrataScriptCore INTERFACE IMPORTED)
	set_target_properties(StrataScriptCore PROPERTIES
		INTERFACE_INCLUDE_DIRECTORIES "${_strataScriptCoreRoot}/Include;${_strataScriptCoreRoot}/../Strata/vendor/glm"
		INTERFACE_COMPILE_DEFINITIONS "GLM_FORCE_DEPTH_ZERO_TO_ONE;GLM_ENABLE_EXPERIMENTAL;GLM_FORCE_SILENT_WARNINGS"
		INTERFACE_COMPILE_FEATURES cxx_std_20)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/StrataScriptModule.cmake")

unset(_strataScriptCoreRoot)
