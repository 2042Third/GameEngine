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

# The instruction sets every engine object is compiled for (strata_engine_cpu_options), Strata's own options. Their
# defaults are the ones the engine was built with while Jolt chose them (x86-64: AVX2 with LZCNT, TZCNT, F16C and FMA;
# no AVX-512), so the code generation of every engine object stayed the same when physics moved to box3d.
# STRATA_CROSS_PLATFORM_DETERMINISTIC leaves out fused multiply-adds (-mfma lets the compiler contract floating-point
# math, which changes results); STRATA_CROSS_COMPILE_ARM targets ARM64 from an x86-64 host. box3d is compiled with its
# own flags (Strata/vendor/CMakeLists.txt), so its results do not depend on these.
option(STRATA_USE_SSE4_1 "Compile the engine for SSE 4.1 (x86-64)" ON)
option(STRATA_USE_SSE4_2 "Compile the engine for SSE 4.2 (x86-64)" ON)
option(STRATA_USE_AVX "Compile the engine for AVX (x86-64)" ON)
option(STRATA_USE_AVX2 "Compile the engine for AVX2 (x86-64)" ON)
option(STRATA_USE_AVX512 "Compile the engine for AVX-512 (x86-64)" OFF)
option(STRATA_USE_LZCNT "Compile the engine for LZCNT (x86-64)" ON)
option(STRATA_USE_TZCNT "Compile the engine for TZCNT (x86-64)" ON)
option(STRATA_USE_F16C "Compile the engine for F16C (x86-64)" ON)
option(STRATA_USE_FMADD "Compile the engine for fused multiply-adds (x86-64)" ON)
option(STRATA_CROSS_PLATFORM_DETERMINISTIC "Compile the engine without fused multiply-adds, for results that match across platforms" OFF)
option(STRATA_CROSS_COMPILE_ARM "Compile the engine for ARM64 from an x86-64 host" OFF)

# strata_engine_cpu_options(<outVariable>)
#   The compile options of the STRATA_USE_* instruction sets: StrataEngineCodegen compiles every engine object with them,
#   also in builds without the physics module, so that leaving a module out changes no other code generation. MSVC gets
#   /arch only with the Visual Studio generators (their platform names the architecture; cl under Ninja gets none), as
#   the engine always had.
function(strata_engine_cpu_options outVariable)
	set(options "")
	if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
		if(CMAKE_VS_PLATFORM_NAME STREQUAL "x86" OR CMAKE_VS_PLATFORM_NAME STREQUAL "x64")
			if(STRATA_USE_AVX512)
				list(APPEND options /arch:AVX512)
			elseif(STRATA_USE_AVX2)
				list(APPEND options /arch:AVX2)
			elseif(STRATA_USE_AVX)
				list(APPEND options /arch:AVX)
			endif()
		endif()
	elseif(XCODE OR STRATA_CROSS_COMPILE_ARM OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
		# Nothing: Xcode builds several architectures at once, and ARM64 needs no options.
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|x86|i386)$")
		if(STRATA_USE_AVX512)
			list(APPEND options -mavx512f -mavx512vl -mavx512dq -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c)
		elseif(STRATA_USE_AVX2)
			list(APPEND options -mavx2 -mbmi -mpopcnt -mlzcnt -mf16c)
		elseif(STRATA_USE_AVX)
			list(APPEND options -mavx -mpopcnt)
		elseif(STRATA_USE_SSE4_2)
			list(APPEND options -msse4.2 -mpopcnt)
		elseif(STRATA_USE_SSE4_1)
			list(APPEND options -msse4.1)
		else()
			list(APPEND options -msse2)
		endif()
		if(STRATA_USE_LZCNT)
			list(APPEND options -mlzcnt)
		endif()
		if(STRATA_USE_TZCNT)
			list(APPEND options -mbmi)
		endif()
		if(STRATA_USE_F16C)
			list(APPEND options -mf16c)
		endif()
		if(STRATA_USE_FMADD AND NOT STRATA_CROSS_PLATFORM_DETERMINISTIC)
			list(APPEND options -mfma)
		endif()
		if(NOT MSVC)
			list(APPEND options -mfpmath=sse)
		endif()
	endif()
	if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
		list(APPEND options -pthread)
	endif()
	list(REMOVE_DUPLICATES options)
	set(${outVariable} "${options}" PARENT_SCOPE)
endfunction()

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
