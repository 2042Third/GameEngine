# The Strata engine: one static library per layer of the layer table (StrataTests/Architecture/Layers.json), made by
# strata_add_layer_libraries (CMake/StrataLayers.cmake): the table is the build. Strata (INTERFACE) links every layer a
# game contains, StrataTooling (INTERFACE) the tooling layers (the asset pipeline, the network, ImGui).

# Code generation every engine object shares, whichever layer compiles it: the instruction sets of Strata's STRATA_USE_*
# options (/arch:AVX2 on MSVC x64 with the Visual Studio generators, nothing with cl under Ninja; -mavx2 -mbmi -mpopcnt
# -mlzcnt -mf16c -mfma -mfpmath=sse on x86 GCC and Clang; -pthread off Windows), from strata_engine_cpu_options
# (CMake/StrataCompilerOptions.cmake), so that builds without the physics module generate the same code. box3d is
# compiled with its own flags (Strata/vendor/CMakeLists.txt), not these.
strata_engine_cpu_options(STRATA_ENGINE_CPU_OPTIONS)
add_library(StrataEngineCodegen INTERFACE)
target_compile_options(StrataEngineCodegen INTERFACE ${STRATA_ENGINE_CPU_OPTIONS})

strata_add_layer_libraries(
	TABLE "${PROJECT_SOURCE_DIR}/StrataTests/Architecture/Layers.json"
	SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src"
	LIBRARIES_FILE "${CMAKE_CURRENT_BINARY_DIR}/LayerLibraries.json")

# Every layer reaches the engine's headers and the generated ones through Core.
target_include_directories(Strata_Core PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${CMAKE_CURRENT_BINARY_DIR}/Generated")
# Which optional modules the engine has (STRATA_WITH_*), 1 or 0, for every target that links the engine: definitions rather
# than a generated header, so that no file can read one as 0 for want of an include.
target_compile_definitions(Strata_Core PUBLIC
	"ST_WITH_PHYSICS=$<BOOL:${STRATA_WITH_PHYSICS}>"
	"ST_WITH_AUDIO=$<BOOL:${STRATA_WITH_AUDIO}>"
	"ST_WITH_ANIMATION=$<BOOL:${STRATA_WITH_ANIMATION}>")

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
add_dependencies(Strata_Core StrataVersionCommit)

# Vendor and system libraries, attached to the layers that use them. A layer that needs another adds a row here.
strata_layer_link(Core PUBLIC glm EnTT spdlog::spdlog nlohmann_json::nlohmann_json)
if(STRATA_ENABLE_TRACY)
	target_compile_definitions(Strata_Core PUBLIC ST_ENABLE_TRACY)
	strata_layer_link(Core PUBLIC Tracy::TracyClient)
endif()
if(WIN32)
	strata_layer_link(Core PRIVATE shell32 ole32 bcrypt dbghelp)
	strata_layer_link(Network PRIVATE ws2_32)
elseif(APPLE)
	strata_layer_link(Core PRIVATE "-framework CoreFoundation")
else()
	find_package(Threads REQUIRED)
	strata_layer_link(Core PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
strata_layer_link(App PRIVATE glfw Vulkan::Headers stb)
strata_layer_link(Asset PRIVATE zstd_decompress)
strata_layer_link(Renderer PUBLIC nvrhi)
strata_layer_link(Renderer PRIVATE nvrhi_vk Vulkan::Headers glfw stb MikkTSpace meshoptimizer)
strata_layer_link(Physics PRIVATE box3d)
strata_layer_link(Audio PRIVATE miniaudio)
# Only the C ABI header (StrataScript/ScriptABI.h); the engine never includes the script SDK.
strata_layer_link(Scripting PRIVATE StrataScriptCore)
strata_layer_link(ImGui PUBLIC imgui)
# Only the asset pack writer (tooling) references compression: games link none of it.
strata_layer_link(AssetPipeline PRIVATE stb cgltf meshoptimizer zstd_compress)

# Engine shaders, compiled to SPIR-V at build time and embedded in the renderer (ImGui's too: one registry per build).
strata_add_shaders(Strata_Renderer
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
		Scene/LightCull.comp
		Scene/LightCompact.comp
		Scene/Cull.comp
		Scene/CullBuild.comp
		Scene/CullFeedback.comp
		Scene/CascadeFit.comp
		Scene/HZBBuild.comp
		Scene/MotionCopy.comp
		Debug/LODSelection.comp
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
		PostProcess/TAA.comp
		PostProcess/FXAA.frag
		PostProcess/Copy.frag
		Overlay/Grid.frag
		Overlay/Outline.frag
		Overlay/StatusTint.frag
		Overlay/MotionView.frag
		Overlay/DebugLine.vert
		Overlay/DebugLine.frag
		Overlay/Text.vert
		Overlay/Text.frag
		GameUI/UIQuad.vert
		GameUI/UIQuad.frag
	VARIANTS
		"Scene/Forward.frag|Scene/ForwardTransparent.frag|ST_FORWARD_TRANSPARENT"
		"Scene/DepthPrepass.frag|Scene/DepthPrepassNoEntityIDs.frag|ST_NO_ENTITY_IDS"
		"Scene/DepthPrepass.frag|Scene/DepthPrepassVelocity.frag|ST_VELOCITY"
		"Scene/DepthPrepass.frag|Scene/DepthPrepassNoEntityIDsVelocity.frag|ST_NO_ENTITY_IDS,ST_VELOCITY"
		"Scene/Mesh.vert|Scene/MeshSkinned.vert|ST_SKINNED"
		"Scene/Mesh.vert|Scene/MeshVelocity.vert|ST_VELOCITY"
		"Scene/Mesh.vert|Scene/MeshSkinnedVelocity.vert|ST_SKINNED,ST_VELOCITY"
		"Scene/ShadowMesh.vert|Scene/ShadowMeshSkinned.vert|ST_SKINNED"
		"Scene/Cull.comp|Scene/CullShadow.comp|ST_CULL_SHADOW"
		"Scene/HZBBuild.comp|Scene/HZBBuildArray.comp|ST_HZB_ARRAY"
		"Effects/GTAO.comp|Effects/GTAOHalf.comp|ST_AO_HALF"
		"Effects/AOBlur.comp|Effects/AOBlurHalf.comp|ST_AO_HALF")

# The default font of text rendering (Roboto Medium, Apache License 2.0; see ThirdPartyNotices.md).
strata_embed_file(Strata_Renderer "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/misc/fonts/Roboto-Medium.ttf" GetDefaultFont)

# What programs link: Strata, every layer a game contains, and StrataTooling, the layers only tools contain.
add_library(Strata INTERFACE)
foreach(layer IN LISTS STRATA_RUNTIME_LAYERS)
	target_link_libraries(Strata INTERFACE Strata_${layer})
endforeach()
add_library(StrataTooling INTERFACE)
foreach(layer IN LISTS STRATA_TOOLING_LAYERS)
	target_link_libraries(StrataTooling INTERFACE Strata_${layer})
endforeach()
set(STRATA_RUNTIME_LAYERS "${STRATA_RUNTIME_LAYERS}" PARENT_SCOPE)
set(STRATA_TOOLING_LAYERS "${STRATA_TOOLING_LAYERS}" PARENT_SCOPE)
