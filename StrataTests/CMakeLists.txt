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
target_link_libraries(StrataTests PRIVATE Strata doctest stb)
if(TARGET StrataEditorCore)
	target_link_libraries(StrataTests PRIVATE StrataEditorCore)
endif()
target_include_directories(StrataTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(StrataTests PRIVATE
	"STRATA_TEST_LIBRARY_NAME=\"$<TARGET_FILE_NAME:StrataTestLibrary>\""
	"STRATA_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
add_dependencies(StrataTests StrataTestLibrary)

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
