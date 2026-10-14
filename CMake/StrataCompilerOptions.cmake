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

# strata_jolt_instruction_sets()
#   Sets STRATA_USE_<NAME> in the caller's scope for each of Jolt's instruction-set options (USE_SSE4_1, USE_SSE4_2,
#   USE_AVX, USE_AVX2, USE_AVX512, USE_LZCNT, USE_TZCNT, USE_F16C, USE_FMADD) and STRATA_CROSS_PLATFORM_DETERMINISTIC: the
#   option's value when it is set (Jolt's option() or the command line), else Jolt's default
#   (Strata/vendor/JoltPhysics/Build/CMakeLists.txt). Builds without the physics module have no Jolt and read the same.
function(strata_jolt_instruction_sets)
	foreach(option IN ITEMS USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT USE_F16C USE_FMADD
			CROSS_PLATFORM_DETERMINISTIC CROSS_COMPILE_ARM)
		if(DEFINED ${option})
			set(value ${${option}})
		elseif(option MATCHES "^(USE_AVX512|CROSS_PLATFORM_DETERMINISTIC|CROSS_COMPILE_ARM)$")
			set(value OFF)
		else()
			set(value ON)
		endif()
		if(value)
			set(STRATA_${option} ON PARENT_SCOPE)
		else()
			set(STRATA_${option} OFF PARENT_SCOPE)
		endif()
	endforeach()
endfunction()

# strata_engine_cpu_options(<outVariable>)
#   The instruction-set options Jolt gives every target that links it (Jolt/Jolt.cmake), under Jolt's own conditions and
#   from strata_jolt_instruction_sets: StrataEngineCodegen compiles every engine object with them, also in builds without
#   the physics module, so that leaving a module out changes no other code generation (-mfma, for one, lets the compiler
#   contract floating-point math, which changes results). Strata/CMakeLists.txt checks them against Jolt's whenever Jolt
#   is built, so the two cannot drift apart.
function(strata_engine_cpu_options outVariable)
	strata_jolt_instruction_sets()
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
		# Jolt sets nothing: Xcode builds several architectures at once, and ARM64 needs no options.
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
