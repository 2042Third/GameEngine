# Build-time compilation of the engine's shaders to embedded SPIR-V (Docs/DeveloperGuide.md, "Shaders").
#
# strata_add_shaders(<target>
#     SOURCE_DIR <dir>
#     SHADERS <registered name>...
#     [VARIANTS "<registered name of the source>|<registered name>|<DEFINE>[,<DEFINE>...]" ...]
#     [INVARIANT_POSITION <registered name of a source>...]
#     [INCLUDE_DIRS <dir>...])
#
# A registered name is the source's path relative to SOURCE_DIR, or the name given to a variant (the same source compiled
# with extra defines), and ends in its stage: .vert vertex, .frag fragment, .comp compute, .geom geometry. Every entry
# takes two commands: the compiler writes <name>.raw.spv, then StrataShaderTool (CMake/StrataShaderTool) checks it,
# decorates the position Invariant for the entries of an INVARIANT_POSITION source and writes the final <name>.spv and
# <name>.h, the uint32_t array a generated registry embeds into <target>. At run time the shaders are looked up by
# registered name through Strata::EmbeddedShaders (Strata/Renderer/EmbeddedShaders.h). The outputs do not depend on the
# configuration: shader bytes are the same in Debug, Release and Dist.
#
# Shared code lives in the include files under SOURCE_DIR. Every shader depends on all of them, so editing an include
# recompiles all shaders (cheap, and immune to depfile parsing quirks). Only one target per build may embed shaders,
# since the registry defines the lookup functions.
#
# EmbeddedShaders/ShaderEntries.txt (the global property STRATA_SHADER_ENTRIES) lists every entry as
# <registered name>|<source file relative to the repository>|<stage>|<defines, comma-separated>, for CI's checks and the
# tests that compile every entry. Slang (CMake/StrataSlang.cmake), the pinned compiler that replaces glslang, is found
# and checked by every configure; its slangc is the global property STRATA_SLANGC.

include("${CMAKE_CURRENT_LIST_DIR}/StrataSlang.cmake")

# StrataShaderTool, built for the host from CMake/StrataShaderTool: standard library only, none of the engine.
function(strata_add_shader_tool)
	if(TARGET StrataShaderTool)
		return()
	endif()
	set(toolDir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StrataShaderTool")
	add_library(StrataShaderToolCore STATIC "${toolDir}/SpirvModule.cpp" "${toolDir}/SpirvModule.h")
	target_include_directories(StrataShaderToolCore PUBLIC "${toolDir}")
	strata_configure_target(StrataShaderToolCore)
	add_executable(StrataShaderTool "${toolDir}/StrataShaderTool.cpp")
	target_link_libraries(StrataShaderTool PRIVATE StrataShaderToolCore)
	strata_configure_target(StrataShaderTool)
	# Not among the programs in bin/<Config>: it runs only in the build.
	set_target_properties(StrataShaderTool PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
endfunction()

function(strata_add_shaders target)
	cmake_parse_arguments(ARG "" "SOURCE_DIR" "SHADERS;VARIANTS;INVARIANT_POSITION;INCLUDE_DIRS" ${ARGN})
	if(NOT ARG_SOURCE_DIR)
		message(FATAL_ERROR "strata_add_shaders: SOURCE_DIR is required")
	endif()
	if(NOT TARGET glslang-standalone)
		message(FATAL_ERROR "strata_add_shaders: the glslang-standalone target is not available")
	endif()
	strata_find_slang(slangc)
	set_property(GLOBAL PROPERTY STRATA_SLANGC "${slangc}")
	strata_add_shader_tool()

	file(GLOB_RECURSE includeFiles CONFIGURE_DEPENDS "${ARG_SOURCE_DIR}/*.glsl")
	set(outputDir "${CMAKE_CURRENT_BINARY_DIR}/EmbeddedShaders")

	set(includeArgs "-I${ARG_SOURCE_DIR}")
	foreach(dir IN LISTS ARG_INCLUDE_DIRS)
		list(APPEND includeArgs "-I${dir}")
	endforeach()

	set(entries "")
	foreach(shader IN LISTS ARG_SHADERS)
		list(APPEND entries "${shader}|${shader}|")
	endforeach()
	list(APPEND entries ${ARG_VARIANTS})

	# Every INVARIANT_POSITION item must be the source of a vertex entry.
	foreach(source IN LISTS ARG_INVARIANT_POSITION)
		if(NOT source MATCHES "\\.vert$")
			message(FATAL_ERROR "strata_add_shaders: INVARIANT_POSITION names '${source}', which is not a vertex shader")
		endif()
		set(found FALSE)
		foreach(entry IN LISTS entries)
			if(entry MATCHES "^([^|]*)\\|" AND CMAKE_MATCH_1 STREQUAL source)
				set(found TRUE)
			endif()
		endforeach()
		if(NOT found)
			message(FATAL_ERROR "strata_add_shaders: INVARIANT_POSITION names '${source}', which no entry compiles")
		endif()
	endforeach()

	set(generatedHeaders "")
	set(registryIncludes "")
	set(registryEntries "")
	set(registeredNames "")
	set(manifest "")
	foreach(entry IN LISTS entries)
		string(REPLACE "|" ";" parts "${entry}")
		list(LENGTH parts partCount)
		if(partCount LESS 2)
			message(FATAL_ERROR "strata_add_shaders: malformed shader entry '${entry}'")
		endif()
		list(GET parts 0 sourceRelative)
		list(GET parts 1 name)
		set(defines "")
		set(defineArgs "")
		if(partCount GREATER 2)
			list(GET parts 2 defines)
			if(defines)
				string(REPLACE "," ";" defineList "${defines}")
				foreach(define IN LISTS defineList)
					list(APPEND defineArgs "-D${define}")
				endforeach()
			endif()
		endif()

		if(name IN_LIST registeredNames)
			message(FATAL_ERROR "strata_add_shaders: duplicate shader name '${name}'")
		endif()
		list(APPEND registeredNames "${name}")

		if(name MATCHES "\\.vert$")
			set(stage "vertex")
		elseif(name MATCHES "\\.frag$")
			set(stage "fragment")
		elseif(name MATCHES "\\.comp$")
			set(stage "compute")
		elseif(name MATCHES "\\.geom$")
			set(stage "geometry")
		else()
			message(FATAL_ERROR "strata_add_shaders: '${name}' has no stage suffix (.vert, .frag, .comp or .geom)")
		endif()

		set(source "${ARG_SOURCE_DIR}/${sourceRelative}")
		if(NOT EXISTS "${source}")
			message(FATAL_ERROR "strata_add_shaders: shader source '${source}' does not exist")
		endif()
		file(RELATIVE_PATH sourceInRepository "${CMAKE_SOURCE_DIR}" "${source}")

		string(MAKE_C_IDENTIFIER "${name}" identifier)
		set(variableName "g_StrataShader_${identifier}")
		set(rawModule "${outputDir}/${name}.raw.spv")
		set(module "${outputDir}/${name}.spv")
		set(header "${outputDir}/${name}.h")
		get_filename_component(headerDir "${header}" DIRECTORY)
		file(MAKE_DIRECTORY "${headerDir}")
		set(invariantArgument "")
		if(sourceRelative IN_LIST ARG_INVARIANT_POSITION)
			set(invariantArgument "--invariant-position")
		endif()

		add_custom_command(
			OUTPUT "${rawModule}"
			COMMAND glslang-standalone -V --target-env vulkan1.2 --quiet
				${includeArgs} ${defineArgs} -o "${rawModule}" "${source}"
			DEPENDS "${source}" ${includeFiles} glslang-standalone
			COMMENT "Compiling shader ${name}"
			VERBATIM)
		add_custom_command(
			OUTPUT "${module}" "${header}"
			COMMAND StrataShaderTool --input "${rawModule}" --spirv "${module}" --header "${header}"
				--variable "${variableName}" --name "${name}" ${invariantArgument}
			DEPENDS "${rawModule}" StrataShaderTool
			COMMENT "Embedding shader ${name}"
			VERBATIM)

		list(APPEND generatedHeaders "${header}")
		string(APPEND registryIncludes "#include \"${name}.h\"\n")
		string(APPEND registryEntries "\t\t{ \"${name}\", ${variableName}, sizeof(${variableName}) },\n")
		string(APPEND manifest "${name}|${sourceInRepository}|${stage}|${defines}\n")
	endforeach()

	list(LENGTH registeredNames shaderCount)
	if(shaderCount EQUAL 0)
		message(FATAL_ERROR "strata_add_shaders: no shaders given")
	endif()

	set(manifestFile "${outputDir}/ShaderEntries.txt")
	file(CONFIGURE OUTPUT "${manifestFile}" CONTENT "${manifest}")
	set_property(GLOBAL PROPERTY STRATA_SHADER_ENTRIES "${manifestFile}")

	set(registrySource "${outputDir}/EmbeddedShaderRegistry.cpp")
	file(CONFIGURE OUTPUT "${registrySource}" CONTENT
"// Generated by CMake/StrataShaders.cmake - do not edit.
#include <cstddef>
#include <cstdint>

${registryIncludes}
#include \"Strata/Renderer/EmbeddedShaders.h\"

namespace Strata::EmbeddedShaders
{
	static const EmbeddedShader s_Shaders[] = {
${registryEntries}	};

	std::span<const EmbeddedShader> GetAll()
	{
		return s_Shaders;
	}
}
")

	target_sources(${target} PRIVATE ${generatedHeaders} "${registrySource}")
	target_include_directories(${target} PRIVATE "${outputDir}")
	source_group("Generated Shaders" FILES ${generatedHeaders} "${registrySource}")
endfunction()
