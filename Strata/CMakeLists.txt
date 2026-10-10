# Strata engine core (static library).

file(GLOB_RECURSE STRATA_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# Platform backends live in src/Platform/<Platform>/ and are filtered per target OS.
if(WIN32)
	list(FILTER STRATA_SOURCES EXCLUDE REGEX "/src/Platform/(Posix|Linux|MacOS)/")
elseif(APPLE)
	list(FILTER STRATA_SOURCES EXCLUDE REGEX "/src/Platform/(Windows|Linux)/")
	file(GLOB_RECURSE STRATA_MACOS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Platform/MacOS/*.mm")
	list(APPEND STRATA_SOURCES ${STRATA_MACOS_SOURCES})
else()
	list(FILTER STRATA_SOURCES EXCLUDE REGEX "/src/Platform/(Windows|MacOS)/")
endif()

add_library(Strata STATIC ${STRATA_SOURCES})
strata_configure_target(Strata)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_SOURCES})

# The commit the engine is built from (c_EngineCommit: editor.status, About), read from git at configure time; "unknown"
# outside a git checkout. The files git changes when HEAD moves (a commit, a checkout) are configure dependencies, so the
# next build configures again and the header follows.
set(STRATA_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
		WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" RESULT_VARIABLE gitResult OUTPUT_VARIABLE gitCommit
		OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
	if(gitResult EQUAL 0 AND gitCommit MATCHES "^[0-9a-f]+$")
		set(STRATA_GIT_COMMIT "${gitCommit}")
		execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --symbolic-full-name HEAD
			WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE gitReference OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
		foreach(gitFile IN ITEMS HEAD packed-refs ${gitReference})
			execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${gitFile}"
				WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE gitPath OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
			if(gitPath)
				get_filename_component(gitPath "${gitPath}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
				if(EXISTS "${gitPath}")
					set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${gitPath}")
				endif()
			endif()
		endforeach()
	endif()
endif()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Strata/Core/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/Generated/Strata/Core/Version.h" @ONLY)

target_include_directories(Strata PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${CMAKE_CURRENT_BINARY_DIR}/Generated")
target_precompile_headers(Strata PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/stpch.h")

# Engine shaders, compiled to SPIR-V at build time and embedded in the library.
strata_add_shaders(Strata
	SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
	SHADERS
		ImGui.vert
		ImGui.frag
		Debug/ColorTriangle.vert
		Debug/ColorTriangle.frag
		Scene/Mesh.vert
		Scene/DepthPrepass.frag
		Scene/Forward.frag
		Scene/Skybox.frag
		Scene/ShadowMesh.vert
		Scene/ShadowMask.frag
		Scene/LightClusters.comp
		IBL/EquirectToCube.comp
		IBL/ProceduralSky.comp
		IBL/CubeDownsample.comp
		IBL/Irradiance.comp
		IBL/Prefilter.comp
		IBL/BRDFLut.comp
		Effects/GTAO.comp
		Effects/AOBlur.comp
		PostProcess/Fullscreen.vert
		PostProcess/Tonemap.frag
		PostProcess/Histogram.comp
		PostProcess/Exposure.comp
		PostProcess/Bloom.comp
		PostProcess/FXAA.frag
		PostProcess/Copy.frag
		Overlay/Grid.frag
		Overlay/Outline.frag
		Overlay/DebugLine.vert
		Overlay/DebugLine.frag
		Overlay/Text.vert
		Overlay/Text.frag
	VARIANTS
		"Scene/Forward.frag|Scene/ForwardTransparent.frag|ST_FORWARD_TRANSPARENT")

# The default font of text rendering (Roboto Medium, Apache License 2.0; see ThirdPartyNotices.md).
strata_embed_file(Strata "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/misc/fonts/Roboto-Medium.ttf" GetDefaultFont)

target_link_libraries(Strata
	PUBLIC
		glm
		EnTT
		spdlog::spdlog
		nlohmann_json::nlohmann_json
		nvrhi
		imgui
		ImGuizmo
	PRIVATE
		StrataScriptCore # Only the C ABI header (StrataScript/ScriptABI.h); the engine never includes the script SDK
		nvrhi_vk
		Vulkan::Headers
		glfw
		stb
		cgltf
		MikkTSpace
		meshoptimizer
		miniaudio
		Jolt)

if(WIN32)
	target_link_libraries(Strata PRIVATE shell32 ole32 ws2_32 bcrypt)
elseif(APPLE)
	target_link_libraries(Strata PRIVATE "-framework CoreFoundation")
else()
	find_package(Threads REQUIRED)
	target_link_libraries(Strata PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

if(STRATA_ENABLE_TRACY)
	target_compile_definitions(Strata PUBLIC ST_ENABLE_TRACY)
	target_link_libraries(Strata PUBLIC Tracy::TracyClient)
endif()
