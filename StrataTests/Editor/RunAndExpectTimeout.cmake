# Checks RunAndExpect.cmake's clock bound (TIMEOUT_SECONDS): a program that runs past it is stopped and the run fails, with
# the reason, long before the program would have ended by itself.
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
string(FIND "${output}" "did not finish within 2 s" position)
if(position EQUAL -1)
	message(FATAL_ERROR "RunAndExpectTimeout: the failure does not name the bound:\n${output}")
endif()
message("RunAndExpectTimeout: stopped and failed after ${seconds} s")
