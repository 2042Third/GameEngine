# Plays the Tetris sample (Samples/Tetris) through the real editor the way an agent does, then runs the exported game:
#   1. the sample is copied into an empty directory (its .strata cache is never copied);
#   2. the editor builds its scripts, plays the scene and makes moves with simulated input (input.key) from the sample's
#      fixed piece sequence: three hard drops that complete the bottom row, with a rotation there and back and a soft drop
#      on the way. The command script's "expect" conditions check the HUD texts the scripts write (line cleared, exact
#      score, level up), then let gravity end the game, restart it, undo the test's field edits and export the game;
#   3. the exported game runs headless with the scripts.
# Every step must succeed. Commands and expectations: TetrisCommands.json.in (entity IDs are those of the sample's scene).
#
# cmake -DEDITOR=<StrataEditor> -DSAMPLE_DIR=<Samples/Tetris> -DCOMMANDS=<TetrisCommands.json.in> -DWORK_DIR=<directory>
#       -DEXECUTABLE_SUFFIX=<.exe or empty> -P TetrisSample.cmake

foreach(variable EDITOR SAMPLE_DIR COMMANDS WORK_DIR)
	if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
		message(FATAL_ERROR "TetrisSample: ${variable} is required")
	endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/Project")
set(project "${WORK_DIR}/Project")
set(TETRIS_EXPORT_DIR "${WORK_DIR}/Build")

# 1. A clean copy of the sample.
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${SAMPLE_DIR}" "${SAMPLE_DIR}/*" "${SAMPLE_DIR}/.*")
foreach(entry IN LISTS entries)
	if(NOT entry STREQUAL ".strata")
		file(COPY "${SAMPLE_DIR}/${entry}" DESTINATION "${project}")
	endif()
endforeach()
if(NOT EXISTS "${project}/Tetris.stproj" OR NOT EXISTS "${project}/Scripts/TetrisBoard.cpp")
	message(FATAL_ERROR "TetrisSample: '${SAMPLE_DIR}' is not the Tetris sample")
endif()

# 2. Build, play, move, check, export.
configure_file("${COMMANDS}" "${WORK_DIR}/TetrisCommands.json" @ONLY)
execute_process(COMMAND "${EDITOR}" --no-gpu --no-automation --quit-after-commands --project "${project}"
		--commands "${WORK_DIR}/TetrisCommands.json"
	WORKING_DIRECTORY "${WORK_DIR}" OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "TetrisSample: the editor's command script failed (${result})")
endif()
foreach(expected "Tetris: new game with seed 60" "Tetris: cleared 1 line; score 209, lines 1, level 2" "Tetris: game over" "Tetris: new game with seed 61")
	string(FIND "${output}" "${expected}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "TetrisSample: the editor did not print '${expected}'")
	endif()
endforeach()

# 3. The exported game runs its scripts headless (at 60 frames per second: two seconds of play).
set(game "${TETRIS_EXPORT_DIR}/Tetris${EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${game}" OR NOT EXISTS "${TETRIS_EXPORT_DIR}/Tetris.stgame" OR NOT EXISTS "${TETRIS_EXPORT_DIR}/Tetris.stpak")
	message(FATAL_ERROR "TetrisSample: project.export did not write the game into '${TETRIS_EXPORT_DIR}'")
endif()
execute_process(COMMAND "${game}" --headless --frames 120 WORKING_DIRECTORY "${TETRIS_EXPORT_DIR}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "TetrisSample: the exported game failed (${result})")
endif()
foreach(expected "Loaded script module 'TetrisScripts'" "Tetris: new game with seed 60" "Tetris: piece 1 is J, next I")
	string(FIND "${output}" "${expected}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "TetrisSample: the exported game did not print '${expected}'")
	endif()
endforeach()
