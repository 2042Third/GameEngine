# Game scripts of "ThirdPerson": every .cpp and .h file in this directory (and below) belongs to the script module ThirdPersonScripts,
# except in folders whose names start with '.' and in build trees (folders holding a CMakeCache.txt): those are neither
# compiled nor watched by the editor.
# The editor builds it (script.build) with the engine's compiler and configuration and loads it from .strata/Scripts/Bin.
# Keep the module's name and output directory; to build by hand:
#   cmake -S Scripts -B <build directory> -DSTRATA_ENGINE_DIR=<Strata folder> && cmake --build <build directory>
cmake_minimum_required(VERSION 3.25)
project(ThirdPersonScripts CXX)

set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata folder, a source checkout or an installation (the editor sets it)")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${STRATA_ENGINE_DIR}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(ThirdPersonScripts SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
