# Checks that every embedded shader also compiles to HLSL: the route to other platforms' shader compilers stays open
# (Docs/DeveloperGuide.md, "Shaders"). slangc -target hlsl over every entry of the build's shader manifest, no DXC: a
# shader that uses spirv_asm outside a __target_switch with a default case, or a Vulkan-only construct with no HLSL form,
# fails here on every CI job.
#
# cmake -DSLANGC=<slangc> -DENTRIES=<EmbeddedShaders/ShaderEntries.txt> -DREPOSITORY=<repository> -DOUTPUT=<folder>
#       -P TargetHLSL.cmake

# The policies of the repository's minimum: a script run by cmake -P starts with old ones.
cmake_minimum_required(VERSION 3.25)

foreach(required SLANGC ENTRIES REPOSITORY OUTPUT)
	if("${${required}}" STREQUAL "")
		message(FATAL_ERROR "TargetHLSL.cmake needs -D${required}=...")
	endif()
endforeach()
if(NOT EXISTS "${SLANGC}")
	message(FATAL_ERROR "There is no slangc at ${SLANGC}")
endif()
if(NOT EXISTS "${ENTRIES}")
	message(FATAL_ERROR "There is no shader manifest at ${ENTRIES}: configure the build first")
endif()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}")
file(STRINGS "${ENTRIES}" lines)
set(total 0)
set(failures "")
foreach(line IN LISTS lines)
	if(line STREQUAL "")
		continue()
	endif()
	string(REPLACE "|" ";" parts "${line}")
	list(LENGTH parts partCount)
	if(partCount LESS 3)
		message(FATAL_ERROR "A malformed manifest line: '${line}'")
	endif()
	list(GET parts 0 name)
	list(GET parts 1 source)
	list(GET parts 2 stage)
	set(defineArgs "")
	if(partCount GREATER 3)
		list(GET parts 3 defines)
		if(NOT defines STREQUAL "")
			string(REPLACE "," ";" defineList "${defines}")
			foreach(define IN LISTS defineList)
				list(APPEND defineArgs "-D${define}")
			endforeach()
		endif()
	endif()
	string(MAKE_C_IDENTIFIER "${name}" identifier)
	set(target "${OUTPUT}/${identifier}.hlsl")
	math(EXPR total "${total} + 1")
	# The flags that change what the HLSL means (column-major matrices) and the two warnings the build disables
	# (CMake/StrataShaders.cmake says why); no Vulkan binding shifts, which have no HLSL form.
	execute_process(COMMAND "${SLANGC}" "${REPOSITORY}/${source}" -entry main -stage ${stage} -target hlsl
			-matrix-layout-column-major -warnings-disable 39001,41012 -I "${REPOSITORY}/Strata/shaders" ${defineArgs} -o "${target}"
		RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output TIMEOUT 120)
	if(NOT result EQUAL 0 OR NOT EXISTS "${target}")
		string(REGEX MATCH "[^\n]*error[^\n]*" first "${output}")
		if(first STREQUAL "")
			string(REGEX MATCH "^[^\n]*" first "${output}")
		endif()
		list(APPEND failures "${name} (${source}): ${first}")
	endif()
endforeach()

list(LENGTH failures failureCount)
math(EXPR emitted "${total} - ${failureCount}")
if(total EQUAL 0)
	message(FATAL_ERROR "The shader manifest ${ENTRIES} lists no entry")
endif()
if(failureCount GREATER 0)
	list(JOIN failures "\n  " failureText)
	message(FATAL_ERROR "${emitted} of ${total} entries emit HLSL; these do not:\n  ${failureText}")
endif()
message(STATUS "${emitted} of ${total} entries emit HLSL")
