# Runs a command script in the editor without a GPU and passes only if the run fails with exit code 1 and its output
# matches EXPECTED_OUTPUT, so the test notices both a missing error message and a crash after it.
#
#   cmake -DEDITOR=<StrataEditor> -DCOMMANDS=<script.json> -DEXPECTED_OUTPUT=<regex> -P ExpectCommandFailure.cmake

foreach(variable IN ITEMS EDITOR COMMANDS EXPECTED_OUTPUT)
	if(NOT DEFINED ${variable})
		message(FATAL_ERROR "${variable} is not set")
	endif()
endforeach()

execute_process(
	COMMAND "${EDITOR}" --no-gpu --frames 2 --commands "${COMMANDS}"
	RESULT_VARIABLE result
	OUTPUT_VARIABLE output
	ERROR_VARIABLE errors)
string(APPEND output "${errors}")

if(NOT result STREQUAL "1")
	message(FATAL_ERROR "The editor exited with '${result}' instead of 1:\n${output}")
endif()
if(NOT output MATCHES "${EXPECTED_OUTPUT}")
	message(FATAL_ERROR "The output does not match '${EXPECTED_OUTPUT}':\n${output}")
endif()
