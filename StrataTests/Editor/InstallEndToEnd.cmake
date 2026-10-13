# Installs the build into a fresh prefix (the layout of a release archive, CMake/StrataPackaging.cmake) and makes a game with
# the installed editor alone, the way a person who downloaded Strata does:
#   1. `cmake --install` into <WORK_DIR>/Strata; the layout holds what a release promises and nothing else;
#   2. the installed editor reports the version, offers the installed samples and creates a project from the basic3d template;
#   3. a script is written into the project's Scripts folder; the editor builds it with the installed SDK (the build's
#      STRATA_ENGINE_DIR must be the installation, never the source tree the editor was built from), attaches it, plays and
#      exports the game;
#   4. the exported game, with the installed runtime renamed after it, runs headless from its export folder.
#
# cmake -DBUILD_DIR=<build tree> -DCONFIG=<configuration> -DSOURCE_DIR=<repository> -DWORK_DIR=<directory it may empty>
#       -DSCRIPT=<Greeter.cpp> -DVERSION=<project version> -DEXECUTABLE_SUFFIX=<.exe or empty> -DMODULE_SUFFIX=<.dll, .so or .dylib>
#       -P InstallEndToEnd.cmake

foreach(variable BUILD_DIR CONFIG SOURCE_DIR WORK_DIR SCRIPT VERSION MODULE_SUFFIX)
	if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
		message(FATAL_ERROR "InstallEndToEnd: ${variable} is required")
	endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/JsonEscape.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(prefix "${WORK_DIR}/Strata")
set(project "${WORK_DIR}/Project")
set(build "${WORK_DIR}/Build")
strata_json_escape(project_json "${project}")
strata_json_escape(build_json "${build}")
# A command that never answers must fail the run long before CTest's timeout (15 minutes at the editor's 60 frames per second;
# --quit-after-commands ends it as soon as the script is done).
set(max_frames 54000)

# The editor keeps its recent projects, sessions and moved script builds in the work directory, never in the user's data.
# (Script module copies go to the user's runtime directory, which must be private: CMake cannot create one.)
set(ENV{STRATA_RECENT_PROJECTS} "${WORK_DIR}/RecentProjects.json")
set(ENV{STRATA_SCRIPT_BUILDS_DIR} "${WORK_DIR}/ScriptBuilds")
set(ENV{STRATA_SESSION_DIR} "${WORK_DIR}/Sessions")

function(fail message)
	message(FATAL_ERROR "InstallEndToEnd: ${message}")
endfunction()

# 1. The installation.
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${prefix}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	fail("cmake --install failed (${result})")
endif()

# Exactly what a release holds at its top: anything else (a dependency's install rules, a stray file) would ship too.
set(expected_entries
	"Docs" "README.md" "Samples" "Skills" "StrataCLI${EXECUTABLE_SUFFIX}" "StrataEditor${EXECUTABLE_SUFFIX}" "StrataInstall.json"
	"StrataRuntime${EXECUTABLE_SUFFIX}" "StrataScriptCore" "ThirdPartyNotices.md")
list(SORT expected_entries)
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${prefix}" "${prefix}/*")
list(SORT entries)
if(NOT entries STREQUAL expected_entries)
	fail("the installation's top level holds '${entries}', expected '${expected_entries}'")
endif()
foreach(file
		"StrataScriptCore/CMake/StrataScriptCoreConfig.cmake" "StrataScriptCore/CMake/StrataScriptModule.cmake"
		"StrataScriptCore/Include/StrataScript/StrataScript.h" "StrataScriptCore/Include/StrataScript/ScriptABI.h"
		"StrataScriptCore/Source/ScriptModuleEntry.cpp" "StrataScriptCore/ThirdParty/glm/glm/glm.hpp"
		"StrataScriptCore/ThirdParty/glm/glm/gtc/quaternion.hpp" "StrataScriptCore/ThirdParty/glm/copying.txt"
		"Samples/Samples.json" "Samples/Tetris/Tetris.stproj" "Docs/Manual/Scripting.md"
		"Skills/strata-editor-automation/SKILL.md" "Skills/strata-make-a-game/SKILL.md" "Skills/strata-scripting/SKILL.md")
	if(NOT EXISTS "${prefix}/${file}")
		fail("the installation lacks '${file}'")
	endif()
endforeach()
file(GLOB_RECURSE sample_data LIST_DIRECTORIES true RELATIVE "${prefix}/Samples" "${prefix}/Samples/*")
list(FILTER sample_data INCLUDE REGEX "(^|/)\\.strata(/|$)")
if(sample_data)
	fail("the installation's samples carry local editor data: ${sample_data}")
endif()
file(READ "${prefix}/StrataInstall.json" marker)
string(JSON marker_version ERROR_VARIABLE marker_error GET "${marker}" EngineVersion)
if(marker_error OR NOT marker_version STREQUAL VERSION)
	fail("StrataInstall.json names version '${marker_version}' (${marker_error}), expected ${VERSION}")
endif()

# On Windows the editor and the runtime (which every exported game is) are windowed programs, so that no console window
# opens with them; the CLI is a console program. The subsystem is a field of the executable's optional header.
function(check_subsystem file expected)
	if(NOT EXECUTABLE_SUFFIX STREQUAL ".exe")
		return()
	endif()
	file(READ "${file}" header OFFSET 60 LIMIT 4 HEX)
	# e_lfanew, little endian.
	string(REGEX REPLACE "^(..)(..)(..)(..)$" "\\4\\3\\2\\1" header "${header}")
	math(EXPR subsystemOffset "0x${header} + 4 + 20 + 68")
	file(READ "${file}" subsystem OFFSET ${subsystemOffset} LIMIT 2 HEX)
	if(NOT subsystem STREQUAL expected)
		fail("'${file}' has the PE subsystem '${subsystem}', expected '${expected}' (0200 windowed, 0300 console)")
	endif()
endfunction()
check_subsystem("${prefix}/StrataEditor${EXECUTABLE_SUFFIX}" "0200")
check_subsystem("${prefix}/StrataRuntime${EXECUTABLE_SUFFIX}" "0200")
check_subsystem("${prefix}/StrataCLI${EXECUTABLE_SUFFIX}" "0300")

set(editor "${prefix}/StrataEditor${EXECUTABLE_SUFFIX}")
execute_process(COMMAND "${editor}" --version OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
if(NOT result EQUAL 0 OR NOT output MATCHES "Strata Editor ${VERSION}")
	fail("the installed editor's --version printed '${output}' (${result}), expected 'Strata Editor ${VERSION}'")
endif()

# Runs the installed editor with a command script, from outside the build and source trees; the output must contain every
# EXPECT text.
function(run_editor name commands)
	cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "ARGUMENTS;EXPECT")
	set(script "${WORK_DIR}/${name}.json")
	file(WRITE "${script}" "${commands}")
	execute_process(COMMAND "${editor}" --no-gpu --no-automation --quit-after-commands --frames ${max_frames} ${ARG_ARGUMENTS} --commands "${script}"
		WORKING_DIRECTORY "${WORK_DIR}" OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
	message("${output}")
	if(NOT result EQUAL 0)
		fail("the installed editor failed in step '${name}' (${result})")
	endif()
	foreach(expected IN LISTS ARG_EXPECT)
		string(FIND "${output}" "${expected}" position)
		if(position EQUAL -1)
			fail("step '${name}' did not print '${expected}'")
		endif()
	endforeach()
endfunction()

# 2. The version, the samples beside the editor and a project from the template people start with.
run_editor(Create "[
	{ \"command\": \"editor.status\", \"expect\": { \"/engineVersion\": { \"equals\": \"${VERSION}\" } } },
	{ \"command\": \"project.samples\", \"expect\": { \"/samples/0/id\": { \"equals\": \"Tetris\" } } },
	{ \"command\": \"project.create\", \"parameters\": { \"directory\": \"${project_json}\", \"name\": \"Install Game\", \"template\": \"basic3d\" } },
	{ \"command\": \"entity.create\", \"parameters\": { \"name\": \"Greeter Host\" } },
	{ \"command\": \"scene.save\" }
]")
foreach(file "${project}/Scripts/CMakeLists.txt" "${project}/Assets/Scenes/Main.stscene")
	if(NOT EXISTS "${file}")
		fail("project.create did not write '${file}'")
	endif()
endforeach()

file(READ "${project}/Assets/Scenes/Main.stscene" scene)
string(JSON entityCount LENGTH "${scene}" Scene Entities)
set(entity "")
math(EXPR lastEntity "${entityCount} - 1")
foreach(index RANGE ${lastEntity})
	string(JSON name GET "${scene}" Scene Entities ${index} Components Name Name)
	if(name STREQUAL "Greeter Host")
		string(JSON entity GET "${scene}" Scene Entities ${index} ID)
	endif()
endforeach()
if(entity STREQUAL "")
	fail("the saved scene has no 'Greeter Host'")
endif()

# 3. The game's script, built with the installed SDK, played and exported.
file(COPY "${SCRIPT}" DESTINATION "${project}/Scripts")
run_editor(BuildPlayExport "[
	{ \"command\": \"script.build\" },
	{ \"command\": \"script.status\", \"expect\": { \"/toolchain/found\": { \"equals\": true }, \"/toolchain/searchesThisMachine\": { \"equals\": true } } },
	{ \"command\": \"script.add\", \"parameters\": { \"entity\": \"${entity}\", \"class\": \"Greeter\", \"fields\": { \"Greeting\": \"Howdy\" } } },
	{ \"command\": \"scene.save\" },
	{ \"command\": \"play.start\" },
	{ \"command\": \"editor.wait\", \"parameters\": { \"frames\": 3 } },
	{ \"command\": \"component.get\", \"parameters\": { \"entity\": \"${entity}\", \"component\": \"Name\" } },
	{ \"command\": \"play.stop\" },
	{ \"command\": \"project.export\", \"parameters\": { \"directory\": \"${build_json}\" } }
]"
	ARGUMENTS --project "${project}"
	EXPECT "Finding a C++ toolchain on this machine" "\"Name\":\"Howdy from Greeter\"" "Greeter ran on 'Howdy from Greeter'")

# The script build configured with the installation's SDK: its cache names the installation, not the source tree the editor
# was built from (which the build tree, and so this prefix, lie in).
file(GLOB caches "${project}/.strata/Scripts/Build/CMakeCache.txt" "${WORK_DIR}/ScriptBuilds/*/Build/CMakeCache.txt")
list(LENGTH caches cacheCount)
if(NOT cacheCount EQUAL 1)
	fail("expected one script build tree, found '${caches}'")
endif()
file(STRINGS "${caches}" engineEntry REGEX "^STRATA_ENGINE_DIR:[A-Z]+=")
file(STRINGS "${caches}" packageEntry REGEX "^StrataScriptCore_DIR:[A-Z]+=")
string(REGEX REPLACE "^[^=]*=" "" engineDirectory "${engineEntry}")
string(REGEX REPLACE "^[^=]*=" "" packageDirectory "${packageEntry}")
file(REAL_PATH "${prefix}" realPrefix)
file(REAL_PATH "${SOURCE_DIR}" realSource)
foreach(entry engineDirectory packageDirectory)
	if("${${entry}}" STREQUAL "")
		fail("the script build's cache has no ${entry} (${caches})")
	endif()
	file(REAL_PATH "${${entry}}" realEntry)
	string(FIND "${realEntry}/" "${realPrefix}/" position)
	if(NOT position EQUAL 0 OR realEntry STREQUAL realSource OR realEntry STREQUAL "${realSource}/StrataScriptCore/CMake")
		fail("the script build used ${entry} '${${entry}}', not the installation '${prefix}'")
	endif()
endforeach()

# 4. The exported game runs the script headless, with the installed runtime and the notices beside it.
set(game "${build}/Install Game${EXECUTABLE_SUFFIX}")
foreach(file "${game}" "${build}/Install Game.stgame" "${build}/InstallGameScripts${MODULE_SUFFIX}" "${build}/ThirdPartyNotices.md")
	if(NOT EXISTS "${file}")
		fail("the export did not write '${file}'")
	endif()
endforeach()
check_subsystem("${game}" "0200")
execute_process(COMMAND "${game}" --headless --frames 30 --save-directory "${WORK_DIR}/Saves" WORKING_DIRECTORY "${build}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	fail("the exported game failed (${result})")
endif()
string(FIND "${output}" "Greeter ran on 'Howdy from Greeter'" position)
if(position EQUAL -1)
	fail("the exported game did not run its script")
endif()
string(FIND "${output}" "(Strata ${VERSION}," position)
if(position EQUAL -1)
	fail("the exported game did not report Strata ${VERSION}")
endif()
