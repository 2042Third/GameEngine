# Checks strata_resolve_package_version (CMake/StrataPackageVersion.cmake), the version in the release archives' names.
# Run as a script: cmake -P PackageVersionTests.cmake

include("${CMAKE_CURRENT_LIST_DIR}/../../CMake/StrataPackageVersion.cmake")

set(failures 0)

# Expects `requested` to resolve to `expected` for project version `projectVersion`, or to be refused when `expected` is
# empty.
function(expect_package_version projectVersion requested expected)
	strata_resolve_package_version(version error "${projectVersion}" "${requested}")
	if(expected STREQUAL "")
		if(error STREQUAL "" OR NOT version STREQUAL "")
			message(SEND_ERROR "'${requested}' for project version ${projectVersion}: expected a refusal, got '${version}'")
		endif()
	elseif(NOT error STREQUAL "" OR NOT version STREQUAL expected)
		message(SEND_ERROR "'${requested}' for project version ${projectVersion}: expected '${expected}', got '${version}' (${error})")
	endif()
endfunction()

# Empty follows the project's version, so a cache made before a version bump keeps configuring.
expect_package_version("0.4.0" "" "0.4.0")
expect_package_version("0.5.0" "" "0.5.0")
# The project's version, with or without a pre-release suffix.
expect_package_version("0.4.0" "0.4.0" "0.4.0")
expect_package_version("0.4.0" "0.4.0-rc1" "0.4.0-rc1")
expect_package_version("0.4.0" "0.4.0-beta.2" "0.4.0-beta.2")
# Another version, a suffix without text, and digits where the version has dots are refused.
expect_package_version("0.4.0" "0.3.0" "")
expect_package_version("0.4.0" "0.4.0-" "")
expect_package_version("0.4.0" "0x4y0" "")
expect_package_version("0.4.0" "0.4.0.1" "")
