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

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Strata/Core/Version.h.in" "${CMAKE_CURRENT_BINARY_DIR}/Generated/Strata/Core/Version.h" @ONLY)
# The commit the engine is built from (c_EngineCommit: editor.status, About), read from git at every build: the header is
# rewritten only when the commit changed, so only its includers recompile after a commit or a checkout.
find_package(Git QUIET)
set(STRATA_VERSION_COMMIT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/Generated/Strata/Core/VersionCommit.h")
add_custom_target(StrataVersionCommit
	COMMAND "${CMAKE_COMMAND}" "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DOUTPUT=${STRATA_VERSION_COMMIT_HEADER}"
		-P "${PROJECT_SOURCE_DIR}/CMake/StrataVersionCommit.cmake"
	BYPRODUCTS "${STRATA_VERSION_COMMIT_HEADER}"
	COMMENT "Reading the git commit of the build"
	VERBATIM)
add_dependencies(Strata StrataVersionCommit)

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
