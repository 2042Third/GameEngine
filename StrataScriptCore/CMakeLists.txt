# StrataScriptCore: the C ABI between the engine and game script modules, plus the header-only C++ SDK that game
# scripts are written against. Script modules never link the engine; they only see these headers, which keeps them
# isolated from engine internals and makes hot reload safe. The engine itself only includes the C ABI header.

add_library(StrataScriptCore INTERFACE)
target_include_directories(StrataScriptCore INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_link_libraries(StrataScriptCore INTERFACE glm)
target_compile_features(StrataScriptCore INTERFACE cxx_std_20)

# strata_add_script_module(): builds game scripts into a module (also used by game projects through the package in
# CMake/StrataScriptCoreConfig.cmake).
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/StrataScriptModule.cmake")
