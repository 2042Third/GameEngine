# StrataCLI: command-line client for the editor automation API and MCP server (`StrataCLI mcp`).
#
# All logic lives in the StrataCLILib static library so the test suite can link and exercise it; the
# StrataCLI executable is only a thin main().

file(GLOB_RECURSE STRATA_CLI_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/CLI/*.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/CLI/*.cpp")

add_library(StrataCLILib STATIC ${STRATA_CLI_SOURCES})
strata_configure_target(StrataCLILib)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${STRATA_CLI_SOURCES})
target_include_directories(StrataCLILib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(StrataCLILib PUBLIC Strata)

add_executable(StrataCLI "${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp")
strata_configure_target(StrataCLI)
target_link_libraries(StrataCLI PRIVATE StrataCLILib)

if(WIN32)
	target_sources(StrataCLI PRIVATE "${PROJECT_SOURCE_DIR}/CMake/Strata.manifest")
endif()
