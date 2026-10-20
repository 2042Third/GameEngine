# The version in the release archives' names (StrataPackaging.cmake).
#
# `requested` is STRATA_PACKAGE_VERSION: empty for the project's version, or the project's version with a pre-release
# suffix such as -rc1 (the release workflow passes the tag's). An empty default keeps a build directory's cache valid
# when project(Strata VERSION) is bumped; a cached copy of the version would name the old one. Sets `outVersion` to the
# version to use and `outError` to why `requested` cannot be used (empty when it can).
function(strata_resolve_package_version outVersion outError projectVersion requested)
	if(requested STREQUAL "")
		set(${outVersion} "${projectVersion}" PARENT_SCOPE)
		set(${outError} "" PARENT_SCOPE)
		return()
	endif()
	string(REPLACE "." "\\." versionPattern "${projectVersion}")
	if(requested MATCHES "^${versionPattern}(-[0-9A-Za-z.]+)?$")
		set(${outVersion} "${requested}" PARENT_SCOPE)
		set(${outError} "" PARENT_SCOPE)
	else()
		string(CONCAT error "STRATA_PACKAGE_VERSION '${requested}' is not the project's version ${projectVersion} (with an "
			"optional pre-release suffix such as -rc1): leave it empty for the project's version (`cmake -U "
			"STRATA_PACKAGE_VERSION` clears a value a build directory cached), or bump project(Strata VERSION) in CMakeLists.txt")
		set(${outVersion} "" PARENT_SCOPE)
		set(${outError} "${error}" PARENT_SCOPE)
	endif()
endfunction()
