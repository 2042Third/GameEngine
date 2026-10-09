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

	add_library(StrataScriptCore INTERFACE IMPORTED)
	set_target_properties(StrataScriptCore PROPERTIES
		INTERFACE_INCLUDE_DIRECTORIES "${_strataScriptCoreRoot}/Include;${_strataScriptCoreRoot}/../Strata/vendor/glm"
		INTERFACE_COMPILE_FEATURES cxx_std_20)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/StrataScriptModule.cmake")

# Modules are built in the configuration of the engine that loads them, so game projects know the engine's
# configurations (CMake/StrataConfigurations.cmake): Debug, Release and Dist. Dist builds scripts with the Release
# flags, as it does the engine (which only adds its own asserts-off define).
get_property(_strataMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_strataMultiConfig AND NOT "Dist" IN_LIST CMAKE_CONFIGURATION_TYPES)
	set(_strataConfigurations ${CMAKE_CONFIGURATION_TYPES} Dist)
	set(CMAKE_CONFIGURATION_TYPES "${_strataConfigurations}" CACHE STRING "Available build configurations" FORCE)
	unset(_strataConfigurations)
endif()
# project() creates empty flag entries for configurations CMake does not know (e.g. with -DCMAKE_CONFIGURATION_TYPES=Dist),
# so empty counts as unset. unset() drops a normal variable that would hide the cache entry.
foreach(_strataLanguage C CXX)
	if(DEFINED CMAKE_${_strataLanguage}_FLAGS_RELEASE AND "${CMAKE_${_strataLanguage}_FLAGS_DIST}" STREQUAL "")
		unset(CMAKE_${_strataLanguage}_FLAGS_DIST)
		set(CMAKE_${_strataLanguage}_FLAGS_DIST "${CMAKE_${_strataLanguage}_FLAGS_RELEASE}" CACHE STRING
			"Flags used by the ${_strataLanguage} compiler for Dist builds" FORCE)
	endif()
endforeach()
foreach(_strataKind EXE SHARED MODULE STATIC)
	if("${CMAKE_${_strataKind}_LINKER_FLAGS_DIST}" STREQUAL "")
		unset(CMAKE_${_strataKind}_LINKER_FLAGS_DIST)
		set(CMAKE_${_strataKind}_LINKER_FLAGS_DIST "${CMAKE_${_strataKind}_LINKER_FLAGS_RELEASE}" CACHE STRING "Linker flags for Dist builds" FORCE)
	endif()
endforeach()
unset(_strataMultiConfig)
unset(_strataLanguage)
unset(_strataKind)

unset(_strataScriptCoreRoot)
