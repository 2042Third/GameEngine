# StrataScriptCore: the C ABI between the engine and game script modules, plus the header-only C++
# SDK that game scripts are written against. Script modules never link the engine; they only see
# these headers, which keeps them isolated from engine internals and makes hot reload safe.

add_library(StrataScriptCore INTERFACE)
target_include_directories(StrataScriptCore INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_link_libraries(StrataScriptCore INTERFACE glm)
