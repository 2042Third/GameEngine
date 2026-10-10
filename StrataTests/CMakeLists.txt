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
#   NewerSDK    describes itself with a larger StrataScriptModuleAPI, as a module of a newer SDK would
#   Dependent   links StrataTestScriptDependency, a library built into a directory of its own
#   FeatureTest the scripts of the feature test project (FeatureTest/Scripts), which exercise every SDK and host API
#               function in the feature scene (see AGENTS.md, "Testing")
set(STRATA_TEST_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Scripts")
add_library(StrataTestScriptDependency SHARED "${STRATA_TEST_SCRIPTS_DIR}/Dependency/ScriptDependency.cpp")
strata_configure_target(StrataTestScriptDependency)
set_target_properties(StrataTestScriptDependency PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/ScriptDependency"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/$<CONFIG>/ScriptDependency"
	FOLDER "Strata/TestScripts")
strata_add_script_module(StrataTestScriptsDependent SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Dependent/DependentScripts.cpp")
target_link_libraries(StrataTestScriptsDependent PRIVATE StrataTestScriptDependency)
strata_add_script_module(StrataTestScriptsAPI SOURCE_DIR "${STRATA_TEST_SCRIPTS_DIR}/API")
strata_add_script_module(StrataTestScriptsReloadV1 SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Reload/ReloadScripts.cpp")
target_compile_definitions(StrataTestScriptsReloadV1 PRIVATE ST_TEST_RELOAD_VERSION=1)
strata_add_script_module(StrataTestScriptsReloadV2 SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Reload/ReloadScripts.cpp")
target_compile_definitions(StrataTestScriptsReloadV2 PRIVATE ST_TEST_RELOAD_VERSION=2)
strata_add_script_module(StrataTestScriptsFaults SOURCE_DIR "${STRATA_TEST_SCRIPTS_DIR}/Faults")
strata_add_script_module(StrataTestScriptsLoadFault SOURCE_DIR "${STRATA_TEST_SCRIPTS_DIR}/LoadFault")
strata_add_script_module(StrataTestScriptsABIMismatch NO_SDK_ENTRY SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Invalid/ABIMismatch.cpp")
strata_add_script_module(StrataTestScriptsMalformed NO_SDK_ENTRY SOURCES "${STRATA_TEST_SCRIPTS_DIR}/Invalid/Malformed.cpp")
strata_add_script_module(StrataTestScriptsNewerSDK NO_SDK_ENTRY SOURCES "${STRATA_TEST_SCRIPTS_DIR}/NewerSDK/NewerSDKModule.cpp")
set(STRATA_FEATURE_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/FeatureTest")
strata_add_script_module(StrataTestScriptsFeatureTest SOURCE_DIR "${STRATA_FEATURE_TEST_DIR}/Scripts")
set(STRATA_TEST_SCRIPT_MODULES
	StrataTestScriptsAPI
	StrataTestScriptsReloadV1
	StrataTestScriptsReloadV2
	StrataTestScriptsFaults
	StrataTestScriptsLoadFault
	StrataTestScriptsABIMismatch
	StrataTestScriptsMalformed
	StrataTestScriptsNewerSDK
	StrataTestScriptsDependent
	StrataTestScriptsFeatureTest)
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
# DOCTEST_CONFIG_NO_POSIX_SIGNALS: CrashGuard owns the fault signals (and the alternate signal stack) of the process.
# doctest would install its own handlers around every test case and then restore the ones from before, removing the
# guard's handlers and leaving crashes in scripts uncontained (or reported as test failures).
target_compile_definitions(StrataTests PRIVATE
	DOCTEST_CONFIG_NO_POSIX_SIGNALS
	"STRATA_TEST_LIBRARY_NAME=\"$<TARGET_FILE_NAME:StrataTestLibrary>\""
	"STRATA_TEST_SCRIPT_DEPENDENCY=\"$<TARGET_FILE:StrataTestScriptDependency>\""
	"STRATA_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
	"STRATA_FEATURE_TEST_DIR=\"${STRATA_FEATURE_TEST_DIR}\"")
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

# The feature test (FeatureTest/, see AGENTS.md "Testing") runs as its own test; its GPU suite runs with the GPU tests.
set(STRATA_FEATURE_TEST_SUITES "FeatureTest,Editor.FeatureTest")

add_test(NAME StrataTests.Core
	COMMAND StrataTests "--test-suite-exclude=GPU*,EndToEnd*,Package*,${STRATA_FEATURE_TEST_SUITES}" --no-intro=true
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")

# Suites whose names start with "Package" build script modules with the real toolchain (CMake and the compiler).
add_test(NAME StrataTests.Package
	COMMAND StrataTests "--test-suite=Package*" --no-intro=true
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")
set_tests_properties(StrataTests.Package PROPERTIES LABELS "package" TIMEOUT 1800)

add_test(NAME StrataTests.FeatureTest
	COMMAND StrataTests "--test-suite=${STRATA_FEATURE_TEST_SUITES}" --no-intro=true
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")
set_tests_properties(StrataTests.FeatureTest PROPERTIES LABELS "feature")

add_test(NAME StrataTests.GPU
	COMMAND StrataTests --test-suite=GPU* --no-intro=true
	WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")
set_tests_properties(StrataTests.GPU PROPERTIES LABELS "gpu")

# Builds a game project's scripts through the StrataScriptCore package (outside this build, as game projects do) and
# checks that the engine loads the result. The package must give scripts the same glm definitions as the engine's glm
# target ('|'-separated, checked by the package project).
add_test(NAME StrataScriptCore.Package
	COMMAND "${CMAKE_COMMAND}"
		"-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/PackageProject"
		"-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/PackageProject/$<CONFIG>"
		"-DENGINE_DIR=${PROJECT_SOURCE_DIR}"
		"-DEXPECTED_GLM_DEFINITIONS=$<JOIN:$<TARGET_PROPERTY:glm,INTERFACE_COMPILE_DEFINITIONS>,|>"
		"-DGENERATOR=${CMAKE_GENERATOR}"
		"-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
		"-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
		"-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
		"-DCONFIG=$<CONFIG>"
		"-DTEST_EXECUTABLE=$<TARGET_FILE:StrataTests>"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/PackageProject/BuildPackageProject.cmake")
set_tests_properties(StrataScriptCore.Package PROPERTIES LABELS "package")

# The same in the Dist configuration, whatever this build's configuration: CMake does not define Dist, so the package
# must for the scripts of a Dist engine to build.
add_test(NAME StrataScriptCore.PackageDist
	COMMAND "${CMAKE_COMMAND}"
		"-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/PackageProject"
		"-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/PackageProject/DistCheck-$<CONFIG>"
		"-DENGINE_DIR=${PROJECT_SOURCE_DIR}"
		"-DEXPECTED_GLM_DEFINITIONS=$<JOIN:$<TARGET_PROPERTY:glm,INTERFACE_COMPILE_DEFINITIONS>,|>"
		"-DGENERATOR=${CMAKE_GENERATOR}"
		"-DPLATFORM=${CMAKE_GENERATOR_PLATFORM}"
		"-DTOOLSET=${CMAKE_GENERATOR_TOOLSET}"
		"-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
		"-DCONFIG=Dist"
		"-DTEST_EXECUTABLE=$<TARGET_FILE:StrataTests>"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/PackageProject/BuildPackageProject.cmake")
set_tests_properties(StrataScriptCore.PackageDist PROPERTIES LABELS "package")

# The real editor, driven by a command script for a few frames: catches startup, UI and shutdown failures.
if(TARGET StrataEditor)
	# The script saves viewport captures into the build tree (absolute paths: it runs without a project).
	set(STRATA_SMOKE_CAPTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/SmokeCaptures")
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Editor/SmokeCommands.json.in" "${CMAKE_CURRENT_BINARY_DIR}/SmokeCommands.json" @ONLY)
	add_test(NAME StrataEditor.Smoke
		COMMAND StrataEditor --no-automation --frames 120 --windowed --commands "${CMAKE_CURRENT_BINARY_DIR}/SmokeCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.Smoke PROPERTIES LABELS "gpu" FIXTURES_SETUP StrataEditorSmokeCaptures)
	# The smoke script's capture shows the scene (grid, floor, crate), not a black or empty image.
	add_test(NAME StrataEditor.SmokeCaptureCheck COMMAND StrataTests --strata-test-helper=check-image "${STRATA_SMOKE_CAPTURE_DIR}/SmokeViewport.png")
	set_tests_properties(StrataEditor.SmokeCaptureCheck PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataEditorSmokeCaptures)

	# The editor executable carries the strata mark as its icon (StrataEditor.rc.in).
	if(WIN32)
		add_test(NAME StrataEditor.Icon COMMAND StrataTests --strata-test-helper=check-icon "$<TARGET_FILE:StrataEditor>")
	endif()

	# A failing command must fail the process, so scripted runs notice.
	add_test(NAME StrataEditor.FailingScript
		COMMAND StrataEditor --no-gpu --no-automation --frames 2 --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/FailingCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.FailingScript PROPERTIES WILL_FAIL TRUE)

	# Commands that take frames (editor.wait) hold the script; the script must finish within the --frames budget...
	add_test(NAME StrataEditor.WaitingScript
		COMMAND StrataEditor --no-gpu --no-automation --frames 10 --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/WaitCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	# ...or the run fails.
	add_test(NAME StrataEditor.UnfinishedScript
		COMMAND StrataEditor --no-gpu --no-automation --frames 5 --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/UnfinishedCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.UnfinishedScript PROPERTIES WILL_FAIL TRUE)

	# Viewport captures need a GPU: without one the command fails with a clear error and the run with exit code 1.
	add_test(NAME StrataEditor.CaptureWithoutGPU
		COMMAND "${CMAKE_COMMAND}"
			"-DCOMMAND=$<TARGET_FILE:StrataEditor>;--no-gpu;--no-automation;--frames;2;--commands;${CMAKE_CURRENT_SOURCE_DIR}/Editor/NoGpuCaptureCommands.json"
			"-DEXPECTED_OUTPUT=viewport[.]capture failed: viewport[.]capture needs a GPU"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectFailure.cmake"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	# Without --frames a headless editor runs until editor.quit (here at the end of its script); a quit that does not
	# close it runs into the timeout.
	add_test(NAME StrataEditor.QuitScript
		COMMAND StrataEditor --no-gpu --no-automation --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/QuitCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.QuitScript PROPERTIES TIMEOUT 60)
	# A quit before the script's last command fails the run: the rest of the script never ran.
	add_test(NAME StrataEditor.QuitBeforeScriptEnds
		COMMAND StrataEditor --no-gpu --no-automation --commands "${CMAKE_CURRENT_SOURCE_DIR}/Editor/QuitEarlyCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.QuitBeforeScriptEnds PROPERTIES WILL_FAIL TRUE TIMEOUT 60)
endif()

# Editor automation end to end (no GPU): the "EndToEnd" suites start the real editor headless and drive it with StrataCLI
# and its MCP server, the way an AI agent does.
if(TARGET StrataEditor AND TARGET StrataCLI)
	add_test(NAME StrataEditor.Automation
		COMMAND StrataTests --test-suite=EndToEnd* --no-intro=true
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataTests>")
	set_tests_properties(StrataEditor.Automation PROPERTIES
		ENVIRONMENT "STRATA_TEST_EDITOR_PATH=$<TARGET_FILE:StrataEditor>;STRATA_TEST_CLI_PATH=$<TARGET_FILE:StrataCLI>"
		TIMEOUT 600)
endif()

# Editor export to a playable game, then the exported runtime: headless without a GPU (--no-gpu, --headless), then in a
# window with rendering (label gpu).
if(TARGET StrataEditor AND TARGET StrataRuntime)
	set(STRATA_EXPORT_TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/ExportSmoke")
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Editor/ExportCommands.json.in" "${CMAKE_CURRENT_BINARY_DIR}/ExportCommands.json" @ONLY)

	add_test(NAME StrataExport.Clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STRATA_EXPORT_TEST_DIR}")
	set_tests_properties(StrataExport.Clean PROPERTIES FIXTURES_SETUP StrataExportClean)

	add_test(NAME StrataEditor.Export
		COMMAND StrataEditor --no-gpu --no-automation --frames 2 --commands "${CMAKE_CURRENT_BINARY_DIR}/ExportCommands.json"
		WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
	set_tests_properties(StrataEditor.Export PROPERTIES FIXTURES_REQUIRED StrataExportClean FIXTURES_SETUP StrataExportedGame)

	add_test(NAME StrataRuntime.Smoke COMMAND "${STRATA_EXPORT_TEST_DIR}/Build/SmokeGame${CMAKE_EXECUTABLE_SUFFIX}" --headless --frames 30)
	set_tests_properties(StrataRuntime.Smoke PROPERTIES FIXTURES_REQUIRED StrataExportedGame)
	# The exported game's executable (the runtime, renamed) carries the strata mark as its icon (StrataRuntime.rc.in).
	if(WIN32)
		add_test(NAME StrataRuntime.Icon COMMAND StrataTests --strata-test-helper=check-icon "${STRATA_EXPORT_TEST_DIR}/Build/SmokeGame${CMAKE_EXECUTABLE_SUFFIX}")
		set_tests_properties(StrataRuntime.Icon PROPERTIES FIXTURES_REQUIRED StrataExportedGame)
	endif()
	# Headless games run at 60 frames per second, so 180 frames are three seconds of game time (and of the clock), not a burst.
	add_test(NAME StrataRuntime.HeadlessPacing
		COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${STRATA_EXPORT_TEST_DIR}/Build/SmokeGame${CMAKE_EXECUTABLE_SUFFIX}" "-DARGUMENTS=--headless|--frames|180"
			-DMIN_SECONDS=2 -P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/RunAndExpect.cmake")
	set_tests_properties(StrataRuntime.HeadlessPacing PROPERTIES FIXTURES_REQUIRED StrataExportedGame)

	# The exported game in a window renders a few frames from its camera and saves the last one, which must show the scene:
	# not black, not one color, and the blue box covering at least 1% of it.
	set(STRATA_RUNTIME_SCREENSHOT "${STRATA_EXPORT_TEST_DIR}/RuntimeScreenshot.png")
	add_test(NAME StrataRuntime.Render
		COMMAND "${STRATA_EXPORT_TEST_DIR}/Build/SmokeGame${CMAKE_EXECUTABLE_SUFFIX}" --windowed --frames 10 --screenshot "${STRATA_RUNTIME_SCREENSHOT}")
	set_tests_properties(StrataRuntime.Render PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataExportedGame FIXTURES_SETUP StrataRuntimeScreenshot)
	add_test(NAME StrataRuntime.RenderCheck COMMAND StrataTests --strata-test-helper=check-image "${STRATA_RUNTIME_SCREENSHOT}" --dominant blue 1)
	set_tests_properties(StrataRuntime.RenderCheck PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataRuntimeScreenshot)

	# A game whose start scene has no camera shows a message frame: a screenshot of it fails the run.
	add_test(NAME StrataRuntime.NoCameraScreenshot
		COMMAND "${CMAKE_COMMAND}"
			"-DCOMMAND=${STRATA_EXPORT_TEST_DIR}/BuildNoCamera/SmokeGame${CMAKE_EXECUTABLE_SUFFIX};--windowed;--frames;5;--screenshot;${STRATA_EXPORT_TEST_DIR}/NoCameraScreenshot.png"
			"-DEXPECTED_OUTPUT=The screenshot shows the missing-camera message"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/ExpectFailure.cmake")
	set_tests_properties(StrataRuntime.NoCameraScreenshot PROPERTIES LABELS "gpu" FIXTURES_REQUIRED StrataExportedGame)
endif()

# The real executables with the feature test project: the editor opens a copy of it, loads the feature scripts
# (script.load), steps the feature scene (the command script's "expect" conditions require the ball to come to rest on
# the platform, the scripts to run without a crash, and play.stop to restore the scene) and exports it with the module;
# the exported game then runs headless with the scripts. Both must exit cleanly and show that the scripts ran
# (Editor/RunAndExpect.cmake).
if(TARGET StrataEditor AND TARGET StrataRuntime)
	set(STRATA_FEATURE_RUN_DIR "${CMAKE_CURRENT_BINARY_DIR}/FeatureTestRun")
	# @VARIABLES@ first, then the generator expressions (the module's path) per configuration.
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Editor/FeatureCommands.json.in" "${CMAKE_CURRENT_BINARY_DIR}/FeatureCommands.json.configured" @ONLY)
	file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/FeatureCommands-$<CONFIG>.json" INPUT "${CMAKE_CURRENT_BINARY_DIR}/FeatureCommands.json.configured")
	set(STRATA_FEATURE_SCRIPTS_RAN "FeatureTest expected warning: true|Loaded script module 'StrataTestScriptsFeatureTest'")

	add_test(NAME StrataFeatureTest.Clean COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STRATA_FEATURE_RUN_DIR}")
	set_tests_properties(StrataFeatureTest.Clean PROPERTIES FIXTURES_SETUP StrataFeatureTestClean LABELS "feature")

	add_test(NAME StrataFeatureTest.Copy COMMAND "${CMAKE_COMMAND}" -E copy_directory "${STRATA_FEATURE_TEST_DIR}" "${STRATA_FEATURE_RUN_DIR}/Project")
	set_tests_properties(StrataFeatureTest.Copy PROPERTIES FIXTURES_REQUIRED StrataFeatureTestClean FIXTURES_SETUP StrataFeatureTestProject LABELS "feature")

	add_test(NAME StrataEditor.FeatureTest
		COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=$<TARGET_FILE:StrataEditor>"
			"-DARGUMENTS=--no-gpu|--no-automation|--frames|300|--project|${STRATA_FEATURE_RUN_DIR}/Project/FeatureTest.stproj|--commands|${CMAKE_CURRENT_BINARY_DIR}/FeatureCommands-$<CONFIG>.json"
			"-DEXPECT=${STRATA_FEATURE_SCRIPTS_RAN}" -P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/RunAndExpect.cmake")
	set_tests_properties(StrataEditor.FeatureTest PROPERTIES FIXTURES_REQUIRED StrataFeatureTestProject FIXTURES_SETUP StrataFeatureTestGame LABELS "feature")

	# The game runs longer than the in-process feature scenario (200 frames), where nothing may end it: its scripts quit only
	# when a test runner asks them to.
	add_test(NAME StrataRuntime.FeatureTest
		COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${STRATA_FEATURE_RUN_DIR}/Build/FeatureTest${CMAKE_EXECUTABLE_SUFFIX}" "-DARGUMENTS=--headless|--frames|240"
			"-DEXPECT=${STRATA_FEATURE_SCRIPTS_RAN}" -P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/RunAndExpect.cmake")
	set_tests_properties(StrataRuntime.FeatureTest PROPERTIES FIXTURES_REQUIRED StrataFeatureTestGame LABELS "feature")
endif()

# A game's script workflow through the real editor and runtime, as an agent drives it (see Editor/ScriptsEndToEnd.cmake):
# create a project, write a script, script.build, script.add, play, export, run the game headless. Like
# StrataScriptCore.Package it needs CMake and the compiler at test time.
if(TARGET StrataEditor AND TARGET StrataRuntime)
	if(WIN32)
		set(STRATA_SCRIPT_MODULE_SUFFIX ".dll")
	elseif(APPLE)
		set(STRATA_SCRIPT_MODULE_SUFFIX ".dylib")
	else()
		set(STRATA_SCRIPT_MODULE_SUFFIX ".so")
	endif()
	add_test(NAME StrataEditor.Scripts
		COMMAND "${CMAKE_COMMAND}"
			"-DEDITOR=$<TARGET_FILE:StrataEditor>"
			"-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/ScriptsEndToEnd/$<CONFIG>"
			"-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/Editor/ScriptProject/Greeter.cpp"
			"-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
			"-DMODULE_SUFFIX=${STRATA_SCRIPT_MODULE_SUFFIX}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/ScriptsEndToEnd.cmake")
	set_tests_properties(StrataEditor.Scripts PROPERTIES LABELS "package" TIMEOUT 1800)

	# The Tetris sample (Samples/Tetris), played through the real editor with simulated input: its scripts are built, moves
	# from its fixed piece sequence clear a line (checked on the HUD), gravity ends the game, it restarts, and the exported
	# game runs headless (see Editor/TetrisSample.cmake).
	add_test(NAME StrataEditor.Tetris
		COMMAND "${CMAKE_COMMAND}"
			"-DEDITOR=$<TARGET_FILE:StrataEditor>"
			"-DSAMPLE_DIR=${PROJECT_SOURCE_DIR}/Samples/Tetris"
			"-DCOMMANDS=${CMAKE_CURRENT_SOURCE_DIR}/Editor/TetrisCommands.json.in"
			"-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/TetrisSample/$<CONFIG>"
			"-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/Editor/TetrisSample.cmake")
	set_tests_properties(StrataEditor.Tetris PROPERTIES LABELS "package" TIMEOUT 1800)
endif()
