# StrataEditor: the Strata scene editor (ImGui docking UI, gizmos, undo/redo, automation server).
#
# StrataEditorCore holds the UI-independent editor (project and scene state, commands, undo/redo) so the
# tests can drive it headless; StrataEditorUI adds the ImGui interface on top, and the executable runs it.

file(GLOB_RECURSE STRATA_EDITOR_CORE_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Editor/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Editor/*.cpp")

add_library(StrataEditorCore STATIC ${STRATA_EDITOR_CORE_SOURCES})
strata_configure_target(StrataEditorCore)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_EDITOR_CORE_SOURCES})
target_include_directories(StrataEditorCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The engine, and the tooling only editors contain: the asset pipeline and the network (automation).
target_link_libraries(StrataEditorCore PUBLIC Strata Strata::AssetPipeline Strata::Network)
# The game icon's conversion (Editor/GameIcon) resizes with stb_image_resize2.
target_link_libraries(StrataEditorCore PRIVATE stb)
strata_precompile_headers(StrataEditorCore "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorPch.h")

# Script builds (script.build) configure game script modules with the engine's own toolchain and configuration and the
# StrataScriptCore package of this checkout; the values are baked into Editor/ScriptBuildConfig.h as C string literals.
function(strata_escape_c_string variable value)
	string(REPLACE "\\" "\\\\" escaped "${value}")
	string(REPLACE "\"" "\\\"" escaped "${escaped}")
	set(${variable} "${escaped}" PARENT_SCOPE)
endfunction()
strata_escape_c_string(ST_SCRIPT_BUILD_CMAKE "${CMAKE_COMMAND}")
strata_escape_c_string(ST_SCRIPT_BUILD_GENERATOR "${CMAKE_GENERATOR}")
strata_escape_c_string(ST_SCRIPT_BUILD_PLATFORM "${CMAKE_GENERATOR_PLATFORM}")
strata_escape_c_string(ST_SCRIPT_BUILD_TOOLSET "${CMAKE_GENERATOR_TOOLSET}")
strata_escape_c_string(ST_SCRIPT_BUILD_CXX_COMPILER "${CMAKE_CXX_COMPILER}")
strata_escape_c_string(ST_SCRIPT_BUILD_MAKE_PROGRAM "${CMAKE_MAKE_PROGRAM}")
strata_escape_c_string(ST_SCRIPT_BUILD_ENGINE_DIR "${PROJECT_SOURCE_DIR}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/Editor/ScriptBuildConfig.h.in" "${CMAKE_CURRENT_BINARY_DIR}/Generated/Editor/ScriptBuildConfig.h" @ONLY)
target_include_directories(StrataEditorCore PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated")
target_compile_definitions(StrataEditorCore PRIVATE "ST_SCRIPT_BUILD_CONFIGURATION=\"$<CONFIG>\"")

# StrataEditorUI is the ImGui interface on top of the core: EditorLayer, the panels, the widget kit, the Bedrock theme and
# the editor's fonts. It is a library so the tests can draw it without a window or a GPU (StrataTests' ImGuiHarness).
file(GLOB_RECURSE STRATA_EDITOR_UI_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/UI/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/UI/*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Panels/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/Panels/*.cpp")
list(APPEND STRATA_EDITOR_UI_SOURCES
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorHost.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorLayer.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/EditorLayer.cpp")

add_library(StrataEditorUI STATIC ${STRATA_EDITOR_UI_SOURCES})
strata_configure_target(StrataEditorUI)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_EDITOR_UI_SOURCES})
# Dear ImGui (the engine's ImGui layer brings it); the gizmos (ImGuizmo) only the viewport draws.
target_link_libraries(StrataEditorUI PUBLIC StrataEditorCore Strata::ImGui PRIVATE ImGuizmo nfd)
# Its own precompiled header: the core's definitions differ (no ImGui), so it cannot reuse the core's.
strata_precompile_headers(StrataEditorUI "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorPch.h" <imgui.h>)

# The editor's typefaces are compiled in (UI/EditorFonts.cpp), so the editor reads no font files. Licenses and versions:
# ThirdPartyNotices.md. UI/Icons.h is generated from the icon font's codepoint map by Tools/GenerateIconHeader.py.
set(STRATA_EDITOR_FONTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Fonts")
strata_embed_file(StrataEditorUI "${STRATA_EDITOR_FONTS_DIR}/Inter-Regular.ttf" GetInterRegularFont)
strata_embed_file(StrataEditorUI "${STRATA_EDITOR_FONTS_DIR}/Inter-SemiBold.ttf" GetInterSemiBoldFont)
strata_embed_file(StrataEditorUI "${STRATA_EDITOR_FONTS_DIR}/JetBrainsMono-Regular.ttf" GetJetBrainsMonoFont)
strata_embed_file(StrataEditorUI "${STRATA_EDITOR_FONTS_DIR}/Lucide.ttf" GetLucideFont)
# About Strata shows the third-party notices (UI/AboutDialog.cpp).
strata_embed_file(StrataEditorUI "${PROJECT_SOURCE_DIR}/ThirdPartyNotices.md" GetThirdPartyNotices)

set(STRATA_EDITOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorApplication.cpp")
add_executable(StrataEditor ${STRATA_EDITOR_SOURCES})
strata_configure_target(StrataEditor)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_EDITOR_SOURCES})
target_link_libraries(StrataEditor PRIVATE StrataEditorUI)

if(WIN32)
	# A windowed program: no console window opens with it. It keeps main (Engine/EntryPoint.h), and prints to the pipes its
	# callers pass or the terminal that started it (Platform::ConnectStandardStreams).
	set_target_properties(StrataEditor PROPERTIES WIN32_EXECUTABLE TRUE)
	if(MSVC)
		target_link_options(StrataEditor PRIVATE /ENTRY:mainCRTStartup)
	endif()
	target_sources(StrataEditor PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
	# The strata mark as the executable's icon, and version information (Explorer, taskbar, Task Manager).
	set(STRATA_BRAND_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Brand/StrataMark.ico")
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/StrataEditor.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/Generated/StrataEditor.rc" @ONLY)
	target_sources(StrataEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated/StrataEditor.rc")
endif()

# The window icon: the strata mark as raw RGBA at every size of c_EditorIconSizes (Tools/GenerateBrandAssets.py), which
# needs no decoding, and its 256-pixel PNG, decoded at startup for macOS's Dock (EditorApplication.cpp). Declared in
# src/EditorIcon.h.
foreach(size IN ITEMS 16 20 24 28 32 40 48 56 64)
	strata_embed_file(StrataEditor "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Brand/StrataMark${size}.rgba" GetStrataMark${size})
endforeach()
strata_embed_file(StrataEditor "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Brand/StrataMark256.png" GetStrataMark256Png)

# The sample projects next to the executables, where the editor's launcher offers them (Open Sample copies one into a
# folder the user picks). Synced at every build, without local editor data (CMake/StrataCopySamples.cmake).
add_custom_target(StrataSamples
	COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${PROJECT_SOURCE_DIR}/Samples" "-DDESTINATION=$<TARGET_FILE_DIR:StrataEditor>/Samples"
		-P "${PROJECT_SOURCE_DIR}/CMake/StrataCopySamples.cmake"
	COMMENT "Copying the sample projects next to the editor"
	VERBATIM)
add_dependencies(StrataEditor StrataSamples)

set_target_properties(StrataEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")
