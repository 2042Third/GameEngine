# Game scripts of "Tetris": every .cpp and .h file in this directory (and below) belongs to the script module TetrisScripts.
# The editor builds it (script.build) with the engine's compiler and configuration and loads it from .strata/Scripts/Bin.
# Keep the module's name and output directory; to build by hand:
#   cmake -S Scripts -B <build directory> -DSTRATA_ENGINE_DIR=<engine checkout> && cmake --build <build directory>
cmake_minimum_required(VERSION 3.25)
project(TetrisScripts CXX)

set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata engine checkout (the editor sets it)")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${STRATA_ENGINE_DIR}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(TetrisScripts SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
