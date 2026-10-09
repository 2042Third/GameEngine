# Runs a program and checks its outcome: the exit code must be 0, the output (stdout and stderr) must contain every
# expected text, and with MIN_SECONDS the run must have taken at least that many seconds (whole seconds of the clock, so
# leave a second of margin).
#
# cmake -DPROGRAM=<executable> [-DARGUMENTS=<arg>|<arg>|...] [-DEXPECT=<text>|<text>|...] [-DWORKING_DIRECTORY=<dir>]
#       [-DMIN_SECONDS=<seconds>] -P RunAndExpect.cmake
# Arguments and expected texts are separated by '|'.

if(NOT DEFINED PROGRAM OR PROGRAM STREQUAL "")
	message(FATAL_ERROR "RunAndExpect: PROGRAM is required")
endif()
string(REPLACE "|" ";" arguments "${ARGUMENTS}")
string(REPLACE "|" ";" expectations "${EXPECT}")
if(NOT WORKING_DIRECTORY)
	get_filename_component(WORKING_DIRECTORY "${PROGRAM}" DIRECTORY)
endif()

string(TIMESTAMP started "%s" UTC)
execute_process(COMMAND "${PROGRAM}" ${arguments} WORKING_DIRECTORY "${WORKING_DIRECTORY}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
string(TIMESTAMP finished "%s" UTC)
message("${output}")
if(NOT result EQUAL 0)
	message(FATAL_ERROR "RunAndExpect: '${PROGRAM}' failed (${result})")
endif()
if(DEFINED MIN_SECONDS)
	math(EXPR seconds "${finished} - ${started}")
	if(seconds LESS MIN_SECONDS)
		message(FATAL_ERROR "RunAndExpect: '${PROGRAM}' finished after ${seconds} s, sooner than ${MIN_SECONDS} s")
	endif()
endif()
foreach(expected IN LISTS expectations)
	string(FIND "${output}" "${expected}" position)
	if(position EQUAL -1)
		message(FATAL_ERROR "RunAndExpect: '${PROGRAM}' did not print '${expected}'")
	endif()
endforeach()
