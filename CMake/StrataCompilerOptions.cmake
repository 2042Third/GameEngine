# Shared compiler settings for first-party Strata targets, and the compiler cache of every target.

# STRATA_COMPILER_CACHE: every C and C++ compile of the build (the vendor libraries' too) goes through the cache program,
# ccache or sccache (found on PATH, or named by STRATA_COMPILER_CACHE_PROGRAM). The Makefile and Ninja generators honor
# the launcher; the Visual Studio generators do not, so cached Windows builds use the windows-ninja preset.
if(STRATA_COMPILER_CACHE)
	find_program(STRATA_COMPILER_CACHE_PROGRAM NAMES ccache sccache DOC "The compiler cache STRATA_COMPILER_CACHE uses")
	if(NOT STRATA_COMPILER_CACHE_PROGRAM)
		message(FATAL_ERROR "STRATA_COMPILER_CACHE needs ccache or sccache on PATH, or STRATA_COMPILER_CACHE_PROGRAM naming one.")
	endif()
	if(CMAKE_GENERATOR MATCHES "Visual Studio")
		message(FATAL_ERROR "STRATA_COMPILER_CACHE needs a Makefile or Ninja generator (the Visual Studio generators ignore compiler "
			"launchers): on Windows, configure the windows-ninja preset in a Visual Studio developer environment.")
	endif()
	set(CMAKE_C_COMPILER_LAUNCHER "${STRATA_COMPILER_CACHE_PROGRAM}")
	set(CMAKE_CXX_COMPILER_LAUNCHER "${STRATA_COMPILER_CACHE_PROGRAM}")
	message(STATUS "Compiler cache: ${STRATA_COMPILER_CACHE_PROGRAM} (precompiled headers: ${STRATA_PRECOMPILED_HEADERS})")
endif()

# strata_configure_target(<target>)
#   Applies the Strata language standard, configuration defines, and warning policy to <target>.
function(strata_configure_target target)
	target_compile_features(${target} PUBLIC cxx_std_20)

	target_compile_definitions(${target} PRIVATE
		"$<$<CONFIG:Debug>:ST_DEBUG>"
		"$<$<CONFIG:Release>:ST_RELEASE>"
		"$<$<CONFIG:Dist>:ST_DIST>")

	if(MSVC)
		target_compile_options(${target} PRIVATE
			/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline /bigobj
			$<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:/WX>)
		if(CMAKE_GENERATOR MATCHES "Visual Studio")
			target_compile_options(${target} PRIVATE /MP)
		endif()
		target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wno-missing-field-initializers
			$<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:-Werror>)
	endif()

	set_target_properties(${target} PROPERTIES FOLDER "Strata")
endfunction()

# strata_precompile_headers(<target> <header>...)
#   Gives <target> a precompiled header made of <header>s (paths, or <system> headers) when STRATA_PRECOMPILED_HEADERS is
#   on. It is force-included into every source of the target. Sources still include what they use: the lint and builds
#   with the option off compile without it.
function(strata_precompile_headers target)
	if(STRATA_PRECOMPILED_HEADERS)
		target_precompile_headers(${target} PRIVATE ${ARGN})
	endif()
endfunction()

# strata_copy_runtime_resources(<target> <source dir> <destination subdir>)
#   Copies a resource directory next to the target's binary after every build.
function(strata_copy_runtime_resources target sourceDir destinationSubdir)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			"${sourceDir}" "$<TARGET_FILE_DIR:${target}>/${destinationSubdir}"
		COMMENT "Copying ${destinationSubdir} for ${target}"
		VERBATIM)
endfunction()
