# Strata automated tests (doctest).
#
# Test suites whose names start with "GPU" need a Vulkan-capable device; they are registered as a
# separate CTest test labelled "gpu" so machines without a GPU can run `ctest -LE gpu`.

# Small shared library used by the DynamicLibrary tests.
add_library(StrataTestLibrary SHARED "${CMAKE_CURRENT_SOURCE_DIR}/TestLibrary/TestLibrary.cpp")
strata_configure_target(StrataTestLibrary)
set_target_properties(StrataTestLibrary PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# Script modules used by the scripting tests, built with the same compiler and configuration as the engine:
#   API         exercises the entire script API
#   ReloadV1/V2 two versions of one module for the hot reload tests
#   Faults      scripts that crash on purpose; LoadFault crashes while loading
#   ABIMismatch and Malformed are written against the C ABI directly and are refused by the engine
set(STRATA_TEST_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Scripts")
strata_add_script_module(StrataTestScriptsAPI SOURCE_DIR "${STRATA_TEST_SCRIPTS_DIR}/API")
strata_add_script_module(StrataTestScriptsReloadV1 SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Reload/ReloadScripts.cpp")
target_compile_definitions(StrataTestScriptsReloadV1 PRIVATE ST_TEST_RELOAD_VERSION=1)
strata_add_script_module(StrataTestScriptsReloadV2 SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Reload/ReloadScripts.cpp")
target_compile_definitions(StrataTestScriptsReloadV2 PRIVATE ST_TEST_RELOAD_VERSION=2)
strata_add_script_module(StrataTestScriptsFaults SOURCE_DIR "${STRATA_TEST_SCRIPTS_DIR}/Faults")
strata_add_script_module(StrataTestScriptsLoadFault SOURCE_DIR "${STRATA_TEST_SCRIPTS_DIR}/LoadFault")
strata_add_script_module(StrataTestScriptsABIMismatch NO_SDK_ENTRY SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Invalid/ABIMismatch.cpp")
strata_add_script_module(StrataTestScriptsMalformed NO_SDK_ENTRY SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Invalid/Malformed.cpp")
set(STRATA_TEST_SCRIPT_MODULES
	StrataTestScriptsAPI
	StrataTestScriptsReloadV1
	StrataTestScriptsReloadV2
	StrataTestScriptsFaults
	StrataTestScriptsLoadFault
	StrataTestScriptsABIMismatch
	StrataTestScriptsMalformed)
foreach(module IN LISTS STRATA_TEST_SCRIPT_MODULES)
	strata_configure_target(${module})
	set_target_properties(${module} PROPERTIES FOLDER "Strata/TestScripts")
endforeach()

file(GLOB_RECURSE STRATA_TEST_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
# Editor tests need the editor core, which is only built with the editor.
if(NOT TARGET StrataEditorCore)
	file(GLOB_RECURSE STRATA_EDITOR_TEST_SOURCES CONFIGURE_DEPENDS
		"${CMAKE_CURRENT_SOURCE_DIR}/src/Editor/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/src/Editor/*.cpp")
	list(REMOVE_ITEM STRATA_TEST_SOURCES ${STRATA_EDITOR_TEST_SOURCES})
endif()

add_executable(StrataTests ${STRATA_TEST_SOURCES})
strata_configure_target(StrataTests)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_TEST_SOURCES})
# StrataScriptCore: the scripting tests call the script C ABI directly.
target_link_libraries(StrataTests PRIVATE Strata StrataScriptCore doctest stb)
if(TARGET StrataEditorCore)
	target_link_libraries(StrataTests PRIVATE StrataEditorCore)
endif()
target_include_directories(StrataTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(StrataTests PRIVATE
	"STRATA_TEST_LIBRARY_NAME=\"$<TARGET_FILE_NAME:StrataTestLibrary>\""
	"STRATA_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
foreach(module IN LISTS STRATA_TEST_SCRIPT_MODULES)
	string(REPLACE "StrataTestScripts" "" moduleKind "${module}")
	string(TOUPPER "${moduleKind}" moduleKind)
	target_compile_definitions(StrataTests PRIVATE "STRATA_TEST_SCRIPTS_${moduleKind}=\"$<TARGET_FILE_NAME:${module}>\"")
endforeach()
add_dependencies(StrataTests StrataTestLibrary ${STRATA_TEST_SCRIPT_MODULES})

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

# Builds a game project's scripts through the StrataScriptCore package (outside this build, as game projects do) and
# checks that the engine loads the result.
add_test(NAME StrataScriptCore.Package
	COMMAND "${CMAKE_COMMAND}"
		"-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/PackageProject"
		"-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/PackageProject/$<CONFIG>"
		"-DENGINE_DIR=${PROJECT_SOURCE_DIR}"
		"-DGENERATOR=${CMAKE_GENERATOR}"
		"-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
		"-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
		"-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
		"-DCONFIG=$<CONFIG>"
		"-DTEST_EXECUTABLE=$<TARGET_FILE:StrataTests>"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/PackageProject/BuildPackageProject.cmake")
set_tests_properties(StrataScriptCore.Package PROPERTIES LABELS "package")

# The real editor, driven by a command script for a few frames: catches startup, UI and shutdown failures.
if(TARGET StrataEditor)
	add_test(NAME StrataEditor.Smoke
		COMMAND StrataEditor --frames 30 --windowed --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/SmokeCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.Smoke PROPERTIES LABELS "gpu")

	# A failing command must fail the process, so scripted runs notice.
	add_test(NAME StrataEditor.FailingScript
		COMMAND StrataEditor --no-gpu --frames 2 --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/FailingCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.FailingScript PROPERTIES WILL_FAIL TRUE)

	# Commands that take frames (editor.wait) hold the script; the script must finish within the --frames budget...
	add_test(NAME StrataEditor.WaitingScript
		COMMAND StrataEditor --no-gpu --frames 10 --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/WaitCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	# ...or the run fails.
	add_test(NAME StrataEditor.UnfinishedScript
		COMMAND StrataEditor --no-gpu --frames 5 --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/UnfinishedCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.UnfinishedScript PROPERTIES WILL_FAIL TRUE)
endif()

# Editor export to a playable game, then the exported runtime: neither needs a GPU (--no-gpu, headless runtime).
if(TARGET StrataEditor AND TARGET StrataRuntime)
	set(STRATA_EXPORT_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/ExportSmoke")
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Editor/ExportCommands.json.in" "${CMAKE_CURRENT_BINARY_DIR}/ExportCommands.json" @ONLY)

	add_test(NAME StrataExport.Clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STRATA_EXPORT_TEST_DIR}")
	set_tests_properties(StrataExport.Clean PROPERTIES FIXTURES_SETUP StrataExportClean)

	add_test(NAME StrataEditor.Export
		COMMAND StrataEditor --no-gpu --frames 2 --commands "${CMAKE_CURRENT_BINARY_DIR}/ExportCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.Export PROPERTIES FIXTURES_REQUIRED StrataExportClean FIXTURES_SETUP StrataExportedGame)

	add_test(NAME StrataRuntime.Smoke COMMAND "${STRATA_EXPORT_TEST_DIR}/Build/SmokeGame${CMAKE_EXECUTABLE_SUFFIX}" --headless --frames 30)
	set_tests_properties(StrataRuntime.Smoke PROPERTIES FIXTURES_REQUIRED StrataExportedGame)
endif()
