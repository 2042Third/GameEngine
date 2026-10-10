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
target_link_libraries(StrataEditorCore PUBLIC Strata)

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

# StrataEditorUI is the ImGui interface on top of the core: EditorLayer and the panels. It is a library so the tests can
# draw it without a window or a GPU (StrataTests' ImGuiHarness).
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
target_link_libraries(StrataEditorUI PUBLIC StrataEditorCore PRIVATE nfd)

set(STRATA_EDITOR_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorApplication.cpp")
add_executable(StrataEditor ${STRATA_EDITOR_SOURCES})
strata_configure_target(StrataEditor)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_EDITOR_SOURCES})
target_link_libraries(StrataEditor PRIVATE StrataEditorUI)

if(WIN32)
	target_sources(StrataEditor PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
endif()

set_target_properties(StrataEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
	strata_copy_runtime_resources(StrataEditor "${CMAKE_CURRENT_SOURCE_DIR}/Resources" "Resources")
endif()
