# A game project's script build, as a game project sets it up: outside the engine build, through the StrataScriptCore
# package. Built by the StrataScriptCore.Package test (see BuildPackageProject.cmake).
cmake_minimum_required(VERSION 3.25)
project(PackageProjectScripts CXX)

set(STRATA_ENGINE_DIR "" CACHE PATH "The Strata engine checkout")
find_package(StrataScriptCore CONFIG REQUIRED PATHS "${STRATA_ENGINE_DIR}/StrataScriptCore/CMake" NO_DEFAULT_PATH)
strata_add_script_module(PackageProjectScripts SOURCE_DIR Scripts)
