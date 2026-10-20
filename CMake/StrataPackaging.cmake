# The installed layout of Strata and its release archives (`cmake --install`, `cpack`).
#
# An installation is one folder: the executables side by side (exported games copy StrataRuntime from beside the editor,
# the launcher offers the samples beside it), the script SDK game projects build against, the samples, the people's
# manual, the agent skills and a marker that tells the editor it runs from an installation (StrataInstall.json: script
# builds then use this folder's SDK and find a toolchain on the machine, StrataEditor/src/Editor/ScriptToolchain.h):
#
#   StrataEditor, StrataRuntime, StrataCLI   (.exe on Windows)
#   StrataInstall.json, README.md, ThirdPartyNotices.md
#   Samples/                                 Samples.json and the sample projects
#   StrataScriptCore/                        CMake/ (the package), Include/, Source/, ThirdParty/glm/
#   Docs/Manual/                             guides for people making games
#   Skills/                                  the agent skills for making games with the editor
#
# `cpack` packs it as Strata-<version>-<platform>.zip on Windows and .tar.gz on Linux and macOS, with the folder of that
# name at its top. STRATA_PACKAGE_VERSION names the version in the file name: empty for the project's version, or the
# project's version with a pre-release suffix (the release workflow passes the tag's, e.g. 0.4.0-rc1).

if(NOT TARGET StrataEditor OR NOT TARGET StrataRuntime OR NOT TARGET StrataCLI)
	message(STATUS "Strata packaging: the editor, the runtime and the CLI are not all built; no install rules")
	return()
endif()

include(StrataPackageVersion)
set(STRATA_PACKAGE_VERSION "" CACHE STRING
	"The version in the release archive's name: empty for the project's version, or it with a pre-release suffix such as -rc1")
strata_resolve_package_version(_strataPackageVersion _strataPackageVersionError "${PROJECT_VERSION}" "${STRATA_PACKAGE_VERSION}")
if(_strataPackageVersionError)
	message(FATAL_ERROR "${_strataPackageVersionError}")
endif()

# The platform part of the archive's name.
if(WIN32)
	set(STRATA_PACKAGE_PLATFORM "windows")
elseif(APPLE)
	set(STRATA_PACKAGE_PLATFORM "macos")
else()
	set(STRATA_PACKAGE_PLATFORM "linux")
endif()
set(_strataArchitecture "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
	set(_strataArchitecture "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${_strataArchitecture}" _strataArchitecture)
if(_strataArchitecture MATCHES "^(amd64|x86_64|x64)$")
	string(APPEND STRATA_PACKAGE_PLATFORM "-x64")
elseif(_strataArchitecture MATCHES "^(arm64|aarch64)$")
	string(APPEND STRATA_PACKAGE_PLATFORM "-arm64")
else()
	string(APPEND STRATA_PACKAGE_PLATFORM "-${_strataArchitecture}")
endif()
unset(_strataArchitecture)

# The executables, at the installation's top: GameExport and the launcher look beside the editor.
install(TARGETS StrataEditor StrataRuntime StrataCLI RUNTIME DESTINATION .)

# The marker: the editor's script builds use the installation's SDK (FindEngineInstallation).
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/Package/StrataInstall.json" CONTENT "{
	\"Strata\": { \"Format\": \"Installation\", \"Version\": 1 },
	\"EngineVersion\": \"@PROJECT_VERSION@\"
}
" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/Package/StrataInstall.json" DESTINATION .)
install(FILES "${PROJECT_SOURCE_DIR}/ThirdPartyNotices.md" DESTINATION .)
install(FILES "${PROJECT_SOURCE_DIR}/Docs/Releases/PackageReadme.md" DESTINATION . RENAME README.md)

# The samples, without local editor data (.strata), as the StrataSamples target copies them next to the editor.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/Samples/" DESTINATION Samples PATTERN ".strata" EXCLUDE)

# The script SDK: what StrataScriptCoreConfig.cmake and the scripts it builds read. glm, the only library scripts see, comes
# along with its license (its headers only).
install(DIRECTORY
	"${PROJECT_SOURCE_DIR}/StrataScriptCore/CMake"
	"${PROJECT_SOURCE_DIR}/StrataScriptCore/Include"
	"${PROJECT_SOURCE_DIR}/StrataScriptCore/Source"
	DESTINATION StrataScriptCore)
install(DIRECTORY "${PROJECT_SOURCE_DIR}/Strata/vendor/glm/glm" DESTINATION StrataScriptCore/ThirdParty/glm
	FILES_MATCHING PATTERN "*.hpp" PATTERN "*.inl" PATTERN "*.h")
install(FILES "${PROJECT_SOURCE_DIR}/Strata/vendor/glm/copying.txt" DESTINATION StrataScriptCore/ThirdParty/glm)

# The manual for people making games (new scripts point to Docs/Manual/Scripting.md "in the Strata engine's folder") and the
# skills an AI agent uses to make games with the editor.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/Docs/Manual/" DESTINATION Docs/Manual)
foreach(skill IN ITEMS strata-editor-automation strata-make-a-game strata-scripting)
	install(FILES "${PROJECT_SOURCE_DIR}/.claude/skills/${skill}/SKILL.md" DESTINATION "Skills/${skill}")
endforeach()

set(CPACK_PACKAGE_NAME "Strata")
set(CPACK_PACKAGE_VENDOR "Strata")
set(CPACK_PACKAGE_VERSION "${_strataPackageVersion}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_FILE_NAME "Strata-${_strataPackageVersion}-${STRATA_PACKAGE_PLATFORM}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/Packages")
if(WIN32)
	set(CPACK_GENERATOR "ZIP")
else()
	set(CPACK_GENERATOR "TGZ")
endif()
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
# Linux executables keep their full symbol tables, a large share of their size (and of every exported game's runtime);
# packages ship them stripped, as Windows packages ship without PDBs. Crash reports name script modules' functions from
# the modules' own files, which stay as they are built. macOS packages keep theirs: strip would have to re-sign them.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(CPACK_STRIP_FILES ON)
endif()
include(CPack)
