# StrataRuntime: the player of exported games. The editor's export copies it next to the game's manifest
# ("<Game>.stgame") and asset pack, renamed after the game.

file(GLOB_RECURSE STRATA_RUNTIME_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_executable(StrataRuntime ${STRATA_RUNTIME_SOURCES})
strata_configure_target(StrataRuntime)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_RUNTIME_SOURCES})
target_include_directories(StrataRuntime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(StrataRuntime PRIVATE Strata)

if(WIN32)
	target_sources(StrataRuntime PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
endif()

# Exported games ship the third-party notices next to the executable; the export copies them from here.
add_custom_command(TARGET StrataRuntime POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/ThirdPartyNotices.md" "$<TARGET_FILE_DIR:StrataRuntime>/ThirdPartyNotices.md"
	VERBATIM)
