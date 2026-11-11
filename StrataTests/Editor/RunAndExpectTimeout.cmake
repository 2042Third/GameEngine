# Checks RunAndExpect.cmake's clock bound (TIMEOUT_SECONDS): a program that runs past it is stopped and the run fails, with
# the reason, long before the program would have ended by itself. And only such a run is called one: a program that fails
# under the bound without an exit code (it could not be started, or it crashed) fails as itself.
#
# cmake -DRUN_AND_EXPECT=<path of RunAndExpect.cmake> -P RunAndExpectTimeout.cmake

if(NOT DEFINED RUN_AND_EXPECT OR NOT EXISTS "${RUN_AND_EXPECT}")
	message(FATAL_ERROR "RunAndExpectTimeout: RUN_AND_EXPECT must name RunAndExpect.cmake")
endif()
string(TIMESTAMP started "%s" UTC)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${CMAKE_COMMAND}" "-DARGUMENTS=-E|sleep|120" -DTIMEOUT_SECONDS=2 -P "${RUN_AND_EXPECT}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
string(TIMESTAMP finished "%s" UTC)
math(EXPR seconds "${finished} - ${started}")
if(result EQUAL 0)
	message(FATAL_ERROR "RunAndExpectTimeout: a program that ran past TIMEOUT_SECONDS passed (after ${seconds} s)")
endif()
if(seconds GREATER 60)
	message(FATAL_ERROR "RunAndExpectTimeout: the program was not stopped at its bound (${seconds} s)")
endif()
# CMake wraps the message's lines where it likes (a longer path of the program, as on CI's runners, moves the break), so the
# words are looked for with any white space between them.
if(NOT output MATCHES "did[ \t\r\n]+not[ \t\r\n]+finish[ \t\r\n]+within[ \t\r\n]+2[ \t\r\n]+s[ \t\r\n]")
	message(FATAL_ERROR "RunAndExpectTimeout: the failure does not name the bound:\n${output}")
endif()
message("RunAndExpectTimeout: stopped and failed after ${seconds} s")

# A program that cannot be started ends, as a crashed one does, with a text for its result instead of an exit code: the run
# fails with that text and does not say the program ran past its bound. CMake wraps the message's lines where it likes, so
# the words are looked for with anything but letters between them.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROGRAM=${CMAKE_CURRENT_LIST_DIR}/NoSuchProgram" -DTIMEOUT_SECONDS=60 -P "${RUN_AND_EXPECT}"
	OUTPUT_VARIABLE output ERROR_VARIABLE output RESULT_VARIABLE result)
if(result EQUAL 0)
	message(FATAL_ERROR "RunAndExpectTimeout: a program that could not be started passed")
endif()
if(output MATCHES "did[^a-z]+not[^a-z]+finish")
	message(FATAL_ERROR "RunAndExpectTimeout: a program that could not be started is said to have run past its bound:\n${output}")
endif()
if(NOT output MATCHES "failed[^a-z]+[(]")
	message(FATAL_ERROR "RunAndExpectTimeout: the failure of a program that could not be started does not give its reason:\n${output}")
endif()
message("RunAndExpectTimeout: a program that could not be started failed as itself")
