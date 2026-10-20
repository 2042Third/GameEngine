# Plays a sample project (Samples/<Directory>) through the real editor the way an agent does, then runs its exported games:
#   1. the sample is copied into an empty directory (its .strata cache is never copied), refusing a directory that does not
#      hold the named project;
#   2. the editor runs the sample's command script (COMMANDS, configured with the export directories as JSON strings:
#      @EXPORT_DIR_JSON@ and @PRUNED_EXPORT_DIR_JSON@), which builds, plays, checks with "expect" and "until" and exports
#      the game in full and pruned; its output must hold every EDITOR_EXPECT line;
#   3. both exports must hold the game, its .stgame manifest, its .stpak and the script module the manifest names, the
#      pruned pack must not be larger, and the games GAME_RUNS names (full, pruned or both, the default) run headless for
#      GAME_FRAMES frames (with a perf log when PERF_LOG is on: <export>/Perf.csv) with their saves in the work directory,
#      printing every GAME_EXPECT line.
# No output may hold a warning, error or critical line that no ALLOWED_MESSAGES expression matches, nor the editor's one
# that no EDITOR_ALLOWED_MESSAGES expression matches either (each with its reason where the test is declared).
#
# cmake -DEDITOR=<StrataEditor> -DSAMPLE_DIR=<Samples/Name> -DPROJECT_FILE=<Name.stproj> -DGAME=<exported name>
#       -DCOMMANDS=<NameCommands.json.in> -DWORK_DIR=<directory> -DEXECUTABLE_SUFFIX=<.exe or empty>
#       [-DEDITOR_EXPECT=a|b] [-DGAME_EXPECT=a|b] [-DGAME_FRAMES=120] [-DPERF_LOG=ON] [-DALLOWED_MESSAGES=regex|regex]
#       [-DEDITOR_ALLOWED_MESSAGES=regex|regex] [-DGAME_RUNS=full|pruned]
#       [-DPERF_CHECKER=<StrataTests> -DPERF_CHECK=--nonzero|Column|...] (the perf logs checked by check-perf-log)
#       -P SampleProject.cmake
# Lists are separated by '|' (a ';' would split the test's command line), so expressions use character classes, not '|'.

foreach(variable EDITOR SAMPLE_DIR PROJECT_FILE GAME COMMANDS WORK_DIR)
	if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
		message(FATAL_ERROR "SampleProject: ${variable} is required")
	endif()
endforeach()
if(NOT DEFINED GAME_FRAMES OR GAME_FRAMES STREQUAL "")
	set(GAME_FRAMES 120)
endif()
if(NOT DEFINED GAME_RUNS OR GAME_RUNS STREQUAL "")
	set(GAME_RUNS "full|pruned")
endif()
foreach(list EDITOR_EXPECT GAME_EXPECT ALLOWED_MESSAGES EDITOR_ALLOWED_MESSAGES PERF_CHECK GAME_RUNS)
	string(REPLACE "|" ";" ${list} "${${list}}")
endforeach()
foreach(run IN LISTS GAME_RUNS)
	if(NOT run STREQUAL "full" AND NOT run STREQUAL "pruned")
		message(FATAL_ERROR "SampleProject: GAME_RUNS names '${run}'; it lists full, pruned or both")
	endif()
endforeach()
include("${CMAKE_CURRENT_LIST_DIR}/JsonEscape.cmake")

# A command that never answers must fail the run long before CTest's timeout: the editor gives up after this many frames
# (15 minutes at its 60 frames per second; --quit-after-commands ends it as soon as the script is done).
set(max_frames 54000)

# Fails with the lines of `output` that are warnings, errors or critical messages that no expression of ALLOWED_MESSAGES
# or of the lists named after `what` matches.
function(check_messages output what)
	set(allowed_expressions ${ALLOWED_MESSAGES})
	foreach(extra IN LISTS ARGN)
		list(APPEND allowed_expressions ${${extra}})
	endforeach()
	string(REPLACE "\r" "" output "${output}")
	string(REPLACE ";" "\\;" output "${output}")
	string(REPLACE "\n" ";" lines "${output}")
	set(unexpected "")
	foreach(line IN LISTS lines)
		if(NOT line MATCHES "\\] \\[(warning|error|critical)\\] ")
			continue()
		endif()
		set(allowed FALSE)
		foreach(expression IN LISTS allowed_expressions)
			if(line MATCHES "${expression}")
				set(allowed TRUE)
				break()
			endif()
		endforeach()
		if(NOT allowed)
			string(APPEND unexpected "\n  ${line}")
		endif()
	endforeach()
	if(NOT unexpected STREQUAL "")
		message(FATAL_ERROR "SampleProject: ${what} logged messages no allowed expression expects:${unexpected}")
	endif()
endfunction()

# Fails unless `output` holds every line of `expected`.
function(check_expected output expected what)
	foreach(line IN LISTS expected)
		string(FIND "${output}" "${line}" position)
		if(position EQUAL -1)
			message(FATAL_ERROR "SampleProject: ${what} did not print '${line}'")
		endif()
	endforeach()
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/Project")
set(project "${WORK_DIR}/Project")
set(EXPORT_DIR "${WORK_DIR}/Build")
set(PRUNED_EXPORT_DIR "${WORK_DIR}/BuildPruned")

# 1. A clean copy of the sample.
if(NOT EXISTS "${SAMPLE_DIR}/${PROJECT_FILE}")
	message(FATAL_ERROR "SampleProject: '${SAMPLE_DIR}' is not the sample of '${PROJECT_FILE}'")
endif()
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${SAMPLE_DIR}" "${SAMPLE_DIR}/*" "${SAMPLE_DIR}/.*")
foreach(entry IN LISTS entries)
	if(NOT entry STREQUAL ".strata")
		file(COPY "${SAMPLE_DIR}/${entry}" DESTINATION "${project}")
	endif()
endforeach()

# 2. Build, play, check, export.
strata_json_escape(EXPORT_DIR_JSON "${EXPORT_DIR}")
strata_json_escape(PRUNED_EXPORT_DIR_JSON "${PRUNED_EXPORT_DIR}")
get_filename_component(commands_name "${COMMANDS}" NAME_WE)
configure_file("${COMMANDS}" "${WORK_DIR}/${commands_name}.json" @ONLY)
execute_process(COMMAND "${EDITOR}" --no-gpu --no-automation --quit-after-commands --frames ${max_frames} --project "${project}"
		--commands "${WORK_DIR}/${commands_name}.json"
	WORKING_DIRECTORY "${WORK_DIR}" OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "SampleProject: the editor's command script failed (${result})")
endif()
check_expected("${output}" "${EDITOR_EXPECT}" "the editor")
check_messages("${output}" "the editor" EDITOR_ALLOWED_MESSAGES)

# 3. The exported games, full and pruned.
foreach(run full pruned)
	if(run STREQUAL "full")
		set(export_dir "${EXPORT_DIR}")
	else()
		set(export_dir "${PRUNED_EXPORT_DIR}")
	endif()
	set(game "${export_dir}/${GAME}${EXECUTABLE_SUFFIX}")
	set(manifest "${export_dir}/${GAME}.stgame")
	if(NOT EXISTS "${game}" OR NOT EXISTS "${manifest}" OR NOT EXISTS "${export_dir}/${GAME}.stpak")
		message(FATAL_ERROR "SampleProject: project.export did not write the game into '${export_dir}'")
	endif()
	file(READ "${manifest}" manifest_text)
	string(JSON module ERROR_VARIABLE module_error GET "${manifest_text}" "Game" "ScriptModule")
	if(module_error OR module STREQUAL "" OR NOT EXISTS "${export_dir}/${module}")
		message(FATAL_ERROR "SampleProject: '${manifest}' names no script module that the export holds (${module_error})")
	endif()
	list(FIND GAME_RUNS "${run}" run_index)
	if(run_index EQUAL -1)
		continue()
	endif()

	set(arguments --headless --frames ${GAME_FRAMES} --save-directory "${WORK_DIR}/Saves")
	if(PERF_LOG)
		file(REMOVE "${export_dir}/Perf.csv")
		list(APPEND arguments --perf-log "${export_dir}/Perf.csv")
	endif()
	execute_process(COMMAND "${game}" ${arguments} WORKING_DIRECTORY "${export_dir}"
		OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
	message("${output}")
	if(NOT result EQUAL 0)
		message(FATAL_ERROR "SampleProject: the exported game in '${export_dir}' failed (${result})")
	endif()
	check_expected("${output}" "${GAME_EXPECT}" "the exported game in '${export_dir}'")
	check_messages("${output}" "the exported game in '${export_dir}'")
	if(PERF_LOG AND NOT EXISTS "${export_dir}/Perf.csv")
		message(FATAL_ERROR "SampleProject: the exported game in '${export_dir}' wrote no perf log")
	endif()
	if(PERF_LOG AND PERF_CHECKER)
		execute_process(COMMAND "${PERF_CHECKER}" --strata-test-helper=check-perf-log "${export_dir}/Perf.csv" ${GAME_FRAMES} ${PERF_CHECK}
			OUTPUT_VARIABLE check_output ERROR_VARIABLE check_output RESULT_VARIABLE check_result)
		if(NOT check_result EQUAL 0)
			message(FATAL_ERROR "SampleProject: the perf log of the exported game in '${export_dir}' fails its check (${check_result}): ${check_output}")
		endif()
	endif()
endforeach()
file(SIZE "${EXPORT_DIR}/${GAME}.stpak" full_pack)
file(SIZE "${PRUNED_EXPORT_DIR}/${GAME}.stpak" pruned_pack)
if(pruned_pack GREATER full_pack)
	message(FATAL_ERROR "SampleProject: the pruned pack (${pruned_pack} bytes) is larger than the full one (${full_pack} bytes)")
endif()
