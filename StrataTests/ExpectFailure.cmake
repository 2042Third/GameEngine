# Runs a command and passes only if it exits with code EXPECTED_RESULT (1 by default; 3 for a command line the program does
# not understand, CommandLineOptions::c_UsageExitCode) and its output matches EXPECTED_OUTPUT, so a test notices both a
# missing error message and a crash (or a success) after it.
#
#   cmake "-DCOMMAND=<executable>;<argument>;..." -DEXPECTED_OUTPUT=<regex> [-DEXPECTED_RESULT=<code>] -P ExpectFailure.cmake

foreach(variable IN ITEMS COMMAND EXPECTED_OUTPUT)
	if(NOT DEFINED ${variable})
		message(FATAL_ERROR "${variable} is not set")
	endif()
endforeach()

execute_process(
	COMMAND ${COMMAND}
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE errors)
string(APPEND output "${errors}")

if(NOT DEFINED EXPECTED_RESULT)
	set(EXPECTED_RESULT 1)
endif()
if(NOT result STREQUAL "${EXPECTED_RESULT}")
	message(FATAL_ERROR "The command exited with '${result}' instead of ${EXPECTED_RESULT}:\n${output}")
endif()
if(NOT output MATCHES "${EXPECTED_OUTPUT}")
	message(FATAL_ERROR "The output does not match '${EXPECTED_OUTPUT}':\n${output}")
endif()
