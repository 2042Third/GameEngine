# Drives the real editor through a game's script workflow the way an agent does, then runs the exported game:
#   1. the editor creates a project with a scene, an entity and a start scene (command script);
#   2. a script is written into the project's Scripts folder;
#   3. the editor builds the scripts (script.build), attaches the script (script.add), saves, plays and exports;
#   4. the exported game runs headless with the script.
# Every step must succeed, and the output of steps 3 and 4 must show that the script ran.
#
# cmake -DEDITOR=<StrataEditor> -DWORK_DIR=<empty directory> -DSCRIPT=<Greeter.cpp> -DEXECUTABLE_SUFFIX=<.exe or empty>
#       -DMODULE_SUFFIX=<.dll, .so or .dylib>
#       -P ScriptsEndToEnd.cmake

foreach(variable EDITOR WORK_DIR SCRIPT MODULE_SUFFIX)
	if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
		message(FATAL_ERROR "ScriptsEndToEnd: ${variable} is required")
	endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(project "${WORK_DIR}/Project")

# Runs the editor with a command script; the output must contain every EXPECT text.
function(run_editor name commands)
	cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "ARGUMENTS;EXPECT")
	set(script "${WORK_DIR}/${name}.json")
	file(WRITE "${script}" "${commands}")
	execute_process(COMMAND "${EDITOR}" --no-gpu --quit-after-commands ${ARG_ARGUMENTS} --commands "${script}"
		WORKING_DIRECTORY "${WORK_DIR}" OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
	message("${output}")
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "ScriptsEndToEnd: the editor failed in step '${name}' (${result})")
	endif()
	foreach(expected IN LISTS ARG_EXPECT)
		string(FIND "${output}" "${expected}" position)
		if(position EQUAL -1)
			message(FATAL_ERROR "ScriptsEndToEnd: step '${name}' did not print '${expected}'")
		endif()
	endforeach()
endfunction()

# 1. A project with a saved start scene holding one entity.
run_editor(Create "[
	{ \"command\": \"project.create\", \"parameters\": { \"directory\": \"${project}\", \"name\": \"Greeting Game\" } },
	{ \"command\": \"entity.create\", \"parameters\": { \"name\": \"Greeter Host\" } },
	{ \"command\": \"scene.saveAs\", \"parameters\": { \"path\": \"Scenes/Main.stscene\" } },
	{ \"command\": \"project.setStartScene\", \"parameters\": { \"scene\": \"Scenes/Main.stscene\" } }
]")
foreach(file "${project}/Scripts/CMakeLists.txt" "${project}/Scripts/Spinner.cpp" "${project}/Assets/Scenes/Main.stscene")
	if(NOT EXISTS "${file}")
		message(FATAL_ERROR "ScriptsEndToEnd: project.create and scene.saveAs did not write '${file}'")
	endif()
endforeach()

# The entity's ID, read from the saved scene.
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
	message(FATAL_ERROR "ScriptsEndToEnd: the saved scene has no 'Greeter Host'")
endif()

# 2. The game's script.
file(COPY "${SCRIPT}" DESTINATION "${project}/Scripts")

# 3. Build, attach, save, play (the script renames its entity), export.
run_editor(BuildPlayExport "[
	{ \"command\": \"script.build\" },
	{ \"command\": \"script.add\", \"parameters\": { \"entity\": \"${entity}\", \"class\": \"Greeter\", \"fields\": { \"Greeting\": \"Howdy\" } } },
	{ \"command\": \"scene.save\" },
	{ \"command\": \"play.start\" },
	{ \"command\": \"editor.wait\", \"parameters\": { \"frames\": 3 } },
	{ \"command\": \"component.get\", \"parameters\": { \"entity\": \"${entity}\", \"component\": \"Name\" } },
	{ \"command\": \"play.stop\" },
	{ \"command\": \"project.export\", \"parameters\": { \"directory\": \"${WORK_DIR}/Build\" } }
]"
	ARGUMENTS --project "${project}"
	EXPECT "\"Name\":\"Howdy from Greeter\"" "Greeter ran on 'Howdy from Greeter'")

# 4. The exported game runs the script headless.
set(game "${WORK_DIR}/Build/Greeting Game${EXECUTABLE_SUFFIX}")
foreach(file "${game}" "${WORK_DIR}/Build/Greeting Game.stgame" "${WORK_DIR}/Build/GreetingGameScripts${MODULE_SUFFIX}")
	if(NOT EXISTS "${file}")
		message(FATAL_ERROR "ScriptsEndToEnd: the export did not write '${file}'")
	endif()
endforeach()
execute_process(COMMAND "${game}" --headless --frames 30 WORKING_DIRECTORY "${WORK_DIR}/Build"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "ScriptsEndToEnd: the exported game failed (${result})")
endif()
string(FIND "${output}" "Greeter ran on 'Howdy from Greeter'" position)
if(position EQUAL -1)
	message(FATAL_ERROR "ScriptsEndToEnd: the exported game did not run its script")
endif()
