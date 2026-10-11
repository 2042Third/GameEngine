# Repeats a session on a copy of the project, the way editor.activity.export is meant to be used (a regression script):
#   1. the editor creates a basic3d project, which is copied as the session finds it (without its .strata directory);
#   2. the editor runs a session on the project (ReplaySessionCommands.json.in: materials, entities, a prefab and its
#      instance, a copy, an undone and redone deletion, a template entity found by name, an edit while playing, a save),
#      each step using results of earlier ones through "bind" and {"$ref"}, and exports what it did as a command script;
#   3. the editor runs that script on the copy;
#   4. the copy's scene, material and prefab must match the original's, their IDs aside (StrataTests
#      --strata-test-helper=compare-documents).
# Every step must succeed.
#
# cmake -DEDITOR=<StrataEditor> -DTESTS=<StrataTests> -DCOMMANDS=<ReplaySessionCommands.json.in> -DWORK_DIR=<directory>
#       -P ReplaySession.cmake

foreach(variable EDITOR TESTS COMMANDS WORK_DIR)
	if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
		message(FATAL_ERROR "ReplaySession: ${variable} is required")
	endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/JsonEscape.cmake")

# A command that never answers must fail the run long before CTest's timeout (--quit-after-commands ends each run as soon as
# its script is done).
set(max_frames 3600)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(original "${WORK_DIR}/Original")
set(copy "${WORK_DIR}/Copy")
set(session "${WORK_DIR}/Session.json")

function(run_editor step)
	execute_process(COMMAND "${EDITOR}" --no-gpu --no-automation --quit-after-commands --frames ${max_frames} ${ARGN}
		WORKING_DIRECTORY "${WORK_DIR}" OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
	message("${output}")
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "ReplaySession: the editor failed to ${step} (${result})")
	endif()
endfunction()

# 1. The project, and its copy as the session finds it.
strata_json_escape(original_json "${original}")
file(WRITE "${WORK_DIR}/Create.json"
	"[ { \"command\": \"project.create\", \"parameters\": { \"directory\": \"${original_json}\", \"name\": \"ReplayGame\", \"template\": \"basic3d\" } } ]\n")
run_editor("create the project" --commands "${WORK_DIR}/Create.json")
file(MAKE_DIRECTORY "${copy}")
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${original}" "${original}/*" "${original}/.*")
foreach(entry IN LISTS entries)
	if(NOT entry STREQUAL ".strata")
		file(COPY "${original}/${entry}" DESTINATION "${copy}")
	endif()
endforeach()
if(NOT EXISTS "${copy}/ReplayGame.stproj" OR NOT EXISTS "${copy}/Assets/Scenes/Main.stscene")
	message(FATAL_ERROR "ReplaySession: project.create did not make a basic3d project in '${original}'")
endif()

# 2. The session, exported as a command script.
strata_json_escape(REPLAY_SESSION_JSON "${session}")
configure_file("${COMMANDS}" "${WORK_DIR}/SessionCommands.json" @ONLY)
run_editor("run the session" --project "${original}" --commands "${WORK_DIR}/SessionCommands.json")
if(NOT EXISTS "${session}")
	message(FATAL_ERROR "ReplaySession: editor.activity.export did not write '${session}'")
endif()
file(READ "${session}" script)
message("The exported script:\n${script}")

# 3. The replay on the copy.
run_editor("replay the session" --project "${copy}" --commands "${session}")

# 4. Both projects hold the same scene and assets.
set(documents)
foreach(document "Scenes/Main.stscene" "Materials/Crate.stmat" "Prefabs/Crate.stprefab")
	list(APPEND documents "${original}/Assets/${document}" "${copy}/Assets/${document}")
endforeach()
execute_process(COMMAND "${TESTS}" --strata-test-helper=compare-documents ${documents}
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "ReplaySession: the replay on the copy did not make what the session made (${result})")
endif()
