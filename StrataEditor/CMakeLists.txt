# StrataEditor: the Strata scene editor (ImGui docking UI, gizmos, undo/redo, automation server).

file(GLOB_RECURSE STRATA_EDITOR_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StrataEditor ${STRATA_EDITOR_SOURCES})
strata_configure_target(StrataEditor)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_EDITOR_SOURCES})
target_include_directories(StrataEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(StrataEditor PRIVATE Strata nfd)

if(WIN32)
	target_sources(StrataEditor PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
endif()

set_target_properties(StrataEditor PROPERTIES
	VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:StrataEditor>")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Resources")
	strata_copy_runtime_resources(StrataEditor "${CMAKE_CURRENT_SOURCE_DIR}/Resources" "Resources")
endif()
