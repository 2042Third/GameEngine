# strata_add_script_module(<target>
#     [SOURCE_DIR <dir>]          every .cpp/.h/.hpp file below <dir> (recursive) belongs to the module
#     [SOURCES <file>...]         additional source files
#     [OUTPUT_NAME <name>]        file name without extension (default: <target>)
#     [OUTPUT_DIRECTORY <dir>]    output directory (multi-config generators append the configuration)
#     [NO_SDK_ENTRY])             the sources define the module entry points themselves (written against the C ABI
#                                 directly instead of the C++ SDK)
#
# Builds a game script module: a shared library of script classes compiled against StrataScriptCore only (it never
# links the engine). The file is "<name>.dll" on Windows, "<name>.so" on Linux and "<name>.dylib" on macOS. Build it
# with the same compiler and configuration as the engine that loads it.
#
# Requires the StrataScriptCore target: it exists inside the engine build, and game projects get it from the package
# (find_package(StrataScriptCore CONFIG REQUIRED PATHS "<engine>/StrataScriptCore/CMake" NO_DEFAULT_PATH)).

include_guard(GLOBAL)

function(strata_add_script_module target)
	cmake_parse_arguments(PARSE_ARGV 1 ARG "NO_SDK_ENTRY" "SOURCE_DIR;OUTPUT_NAME;OUTPUT_DIRECTORY" "SOURCES")
	if(ARG_UNPARSED_ARGUMENTS)
		message(FATAL_ERROR "strata_add_script_module(${target}): unknown arguments ${ARG_UNPARSED_ARGUMENTS}")
	endif()
	if(NOT TARGET StrataScriptCore)
		message(FATAL_ERROR "strata_add_script_module(${target}): the StrataScriptCore target does not exist; use find_package(StrataScriptCore)")
	endif()

	set(sources ${ARG_SOURCES})
	if(ARG_SOURCE_DIR)
		get_filename_component(sourceDir "${ARG_SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
		if(NOT IS_DIRECTORY "${sourceDir}")
			message(FATAL_ERROR "strata_add_script_module(${target}): SOURCE_DIR '${sourceDir}' is not a directory")
		endif()
		file(GLOB_RECURSE globbedSources CONFIGURE_DEPENDS "${sourceDir}/*.cpp" "${sourceDir}/*.h" "${sourceDir}/*.hpp")
		list(APPEND sources ${globbedSources})
	endif()
	if(NOT sources)
		message(FATAL_ERROR "strata_add_script_module(${target}): no sources (pass SOURCE_DIR or SOURCES)")
	endif()

	set(outputName "${target}")
	if(ARG_OUTPUT_NAME)
		set(outputName "${ARG_OUTPUT_NAME}")
	endif()

	if(WIN32)
		set(suffix ".dll")
	elseif(APPLE)
		set(suffix ".dylib")
	else()
		set(suffix ".so")
	endif()

	# The module entry points (StrataScript_GetABIVersion, StrataScript_Load) come from the SDK.
	if(NOT ARG_NO_SDK_ENTRY)
		list(APPEND sources "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../Source/ScriptModuleEntry.cpp")
	endif()
	add_library(${target} MODULE ${sources})
	target_link_libraries(${target} PRIVATE StrataScriptCore)
	target_compile_features(${target} PRIVATE cxx_std_20)
	target_compile_definitions(${target} PRIVATE "ST_SCRIPT_MODULE_NAME=\"${outputName}\"")

	# Only the entry points are exported; everything else stays private to the module, so modules never interpose on
	# each other's (or the engine's) symbols.
	set_target_properties(${target} PROPERTIES
		PREFIX ""
		SUFFIX "${suffix}"
		OUTPUT_NAME "${outputName}"
		CXX_EXTENSIONS OFF
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON
		POSITION_INDEPENDENT_CODE ON
		MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
		FOLDER "Scripts")
	if(ARG_OUTPUT_DIRECTORY)
		set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${ARG_OUTPUT_DIRECTORY}")
	endif()

	if(MSVC)
		target_compile_options(${target} PRIVATE /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor)
	else()
		if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
			# GCC otherwise emits STB_GNU_UNIQUE symbols for inline statics, which makes dlclose() a no-op and would
			# keep every hot-reloaded module version in memory.
			target_compile_options(${target} PRIVATE -fno-gnu-unique)
		endif()
		if(NOT APPLE)
			# Scripts must not depend on symbols of the process that loads them (macOS bundles fail on them by default).
			target_link_options(${target} PRIVATE "LINKER:--no-undefined")
		endif()
	endif()
endfunction()
