# StrataEditor: the Strata scene editor (ImGui docking UI, gizmos, undo/redo, automation server).
#
# StrataEditorCore holds the UI-independent editor (project and scene state, commands, undo/redo) so the
# tests can drive it headless; the executable adds the ImGui interface on top.

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

file(GLOB_RECURSE STRATA_EDITOR_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM STRATA_EDITOR_SOURCES ${STRATA_EDITOR_CORE_SOURCES})

add_executable(StrataEditor ${STRATA_EDITOR_SOURCES})
strata_configure_target(StrataEditor)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_EDITOR_SOURCES})
target_include_directories(StrataEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(StrataEditor PRIVATE StrataEditorCore nfd)

if(WIN32)
	target_sources(StrataEditor PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
	# The strata mark as the executable's icon, and version information (Explorer, taskbar, Task Manager).
	set(STRATA_BRAND_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Brand/StrataMark.ico")
	configure_file("${CMAKE_CURRENT_SOURCE_DIR}/StrataEditor.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/Generated/StrataEditor.rc" @ONLY)
	target_sources(StrataEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/Generated/StrataEditor.rc")
endif()

# The window icon: the strata mark as raw RGBA at 16, 32 and 48 pixels (Tools/GenerateBrandAssets.py), so nothing is
# decoded at startup. Declared in src/EditorIcon.h.
foreach(size IN ITEMS 16 32 48)
	strata_embed_file(StrataEditor "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Brand/StrataMark${size}.rgba" GetStrataMark${size})
endforeach()

set_target_properties(StrataEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
	strata_copy_runtime_resources(StrataEditor "${CMAKE_CURRENT_SOURCE_DIR}/Resources" "Resources")
endif()
