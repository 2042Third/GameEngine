# Build configurations and global toolchain settings.
#
# Strata uses three configurations (Hazel convention):
#   Debug   - unoptimized, asserts and logging enabled
#   Release - optimized with debug info, asserts and logging enabled (day-to-day development)
#   Dist    - optimized distribution build used for exported games, asserts compiled out

get_property(STRATA_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(STRATA_MULTI_CONFIG)
	set(CMAKE_CONFIGURATION_TYPES "Debug;Release;Dist" CACHE STRING "Available build configurations" FORCE)
else()
	if(NOT CMAKE_BUILD_TYPE)
		set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build configuration (Debug, Release, Dist)" FORCE)
	endif()
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "Dist")
endif()

# Dist uses the Release toolchain flags; only the ST_DIST define differs.
foreach(lang C CXX)
	set(CMAKE_${lang}_FLAGS_DIST "${CMAKE_${lang}_FLAGS_RELEASE}" CACHE STRING "Flags used by the ${lang} compiler for Dist builds" FORCE)
endforeach()
foreach(kind EXE SHARED MODULE STATIC)
	set(CMAKE_${kind}_LINKER_FLAGS_DIST "${CMAKE_${kind}_LINKER_FLAGS_RELEASE}" CACHE STRING "Linker flags for Dist builds" FORCE)
endforeach()
# Imported targets: prefer their Release variant, else the configuration-less location (the empty entry).
set(CMAKE_MAP_IMPORTED_CONFIG_DIST "Release" "")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Third-party projects with old cmake_minimum_required() calls should still honor these.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW) # option() honors normal variables
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW) # MSVC runtime library abstraction
set(CMAKE_POLICY_DEFAULT_CMP0141 NEW) # MSVC debug information format abstraction
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Statically link the MSVC runtime everywhere so exported games need no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase")
if(MSVC)
	add_link_options("$<$<CONFIG:Release,Dist>:/DEBUG;/OPT:REF;/OPT:ICF>")
endif()

# All executables and shared libraries land in build/<preset>/bin[/<Config>].
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
