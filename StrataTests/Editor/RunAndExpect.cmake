# Runs a program and checks its outcome: the exit code must be 0 and the output (stdout and stderr) must contain every
# expected text.
#
# cmake -DPROGRAM=<executable> [-DARGUMENTS=<arg>|<arg>|...] [-DEXPECT=<text>|<text>|...] [-DWORKING_DIRECTORY=<dir>]
#       -P RunAndExpect.cmake
# Arguments and expected texts are separated by '|'.

if(NOT DEFINED PROGRAM OR PROGRAM STREQUAL "")
	message(FATAL_ERROR "RunAndExpect: PROGRAM is required")
endif()
string(REPLACE "|" ";" arguments "${ARGUMENTS}")
string(REPLACE "|" ";" expectations "${EXPECT}")
if(NOT WORKING_DIRECTORY)
	get_filename_component(WORKING_DIRECTORY "${PROGRAM}" DIRECTORY)
endif()

execute_process(COMMAND "${PROGRAM}" ${arguments} WORKING_DIRECTORY "${WORKING_DIRECTORY}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "RunAndExpect: '${PROGRAM}' failed (${result})")
endif()
foreach(expected IN LISTS expectations)
	string(FIND "${output}" "${expected}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "RunAndExpect: '${PROGRAM}' did not print '${expected}'")
	endif()
endforeach()
