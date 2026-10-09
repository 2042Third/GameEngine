# Configures and builds the package project with the engine's generator and configuration, then lets the test
# executable load the module it produced.
#
# cmake -DSOURCE_DIR=... -DBINARY_DIR=... -DENGINE_DIR=... -DGENERATOR=... [-DPLATFORM=...] [-DTOOLSET=...]
#       [-DCXX_COMPILER=...] -DCONFIG=... -DTEST_EXECUTABLE=... -P BuildPackageProject.cmake

foreach(variable SOURCE_DIR BINARY_DIR ENGINE_DIR GENERATOR CONFIG TEST_EXECUTABLE)
	if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
		message(FATAL_ERROR "BuildPackageProject: ${variable} is required")
	endif()
endforeach()

set(configureArguments -S "${SOURCE_DIR}" -B "${BINARY_DIR}" -G "${GENERATOR}" "-DSTRATA_ENGINE_DIR=${ENGINE_DIR}")
if(PLATFORM)
	list(APPEND configureArguments -A "${PLATFORM}")
endif()
if(TOOLSET)
	list(APPEND configureArguments -T "${TOOLSET}")
endif()
if(CXX_COMPILER AND NOT GENERATOR MATCHES "Visual Studio")
	list(APPEND configureArguments "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
if(NOT GENERATOR MATCHES "Visual Studio|Xcode|Multi-Config")
	list(APPEND configureArguments "-DCMAKE_BUILD_TYPE=${CONFIG}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" ${configureArguments} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Configuring the package project failed (${result})")
endif()

# The package defines the engine's Dist configuration with the Release flags (an empty flag set would build unoptimized).
if(CONFIG STREQUAL "Dist")
	file(STRINGS "${BINARY_DIR}/CMakeCache.txt" distFlags REGEX "^CMAKE_CXX_FLAGS_DIST:")
	file(STRINGS "${BINARY_DIR}/CMakeCache.txt" releaseFlags REGEX "^CMAKE_CXX_FLAGS_RELEASE:")
	string(REGEX REPLACE "^[^=]*=" "" distFlags "${distFlags}")
	string(REGEX REPLACE "^[^=]*=" "" releaseFlags "${releaseFlags}")
	if(distFlags STREQUAL "" OR NOT distFlags STREQUAL releaseFlags)
		message(FATAL_ERROR "The package's Dist flags ('${distFlags}') differ from its Release flags ('${releaseFlags}')")
	endif()
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" --config "${CONFIG}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "Building the package project failed (${result})")
endif()

if(WIN32)
	set(moduleFile "PackageProjectScripts.dll")
elseif(APPLE)
	set(moduleFile "PackageProjectScripts.dylib")
else()
	set(moduleFile "PackageProjectScripts.so")
endif()
file(GLOB_RECURSE modules "${BINARY_DIR}/${moduleFile}")
list(LENGTH modules moduleCount)
if(NOT moduleCount EQUAL 1)
	message(FATAL_ERROR "Expected one ${moduleFile} in ${BINARY_DIR}, found: ${modules}")
endif()

execute_process(COMMAND "${TEST_EXECUTABLE}" --strata-test-helper=load-script-module "${modules}" Player Game::Enemy RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "The engine could not load the package project's module (${result})")
endif()
