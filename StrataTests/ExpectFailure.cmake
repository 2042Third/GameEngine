# Runs a command and passes only if it exits with code 1 and its output matches EXPECTED_OUTPUT, so a test notices both a
# missing error message and a crash (or a success) after it.
#
#   cmake "-DCOMMAND=<executable>;<argument>;..." -DEXPECTED_OUTPUT=<regex> -P ExpectFailure.cmake

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

if(NOT result STREQUAL "1")
	message(FATAL_ERROR "The command exited with '${result}' instead of 1:\n${output}")
endif()
if(NOT output MATCHES "${EXPECTED_OUTPUT}")
	message(FATAL_ERROR "The output does not match '${EXPECTED_OUTPUT}':\n${output}")
endif()
