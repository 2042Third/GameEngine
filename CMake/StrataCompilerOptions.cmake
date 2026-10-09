# Shared compiler settings for first-party Strata targets.

# strata_configure_target(<target>)
#   Applies the Strata language standard, configuration defines, and warning policy to <target>.
function(strata_configure_target target)
	target_compile_features(${target} PUBLIC cxx_std_20)

	target_compile_definitions(${target} PRIVATE
		"$<$<CONFIG:Debug>:ST_DEBUG>"
		"$<$<CONFIG:Release>:ST_RELEASE>"
		"$<$<CONFIG:Dist>:ST_DIST>")

	if(MSVC)
		target_compile_options(${target} PRIVATE
			/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /Zc:inline /bigobj
			$<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:/WX>)
		if(CMAKE_GENERATOR MATCHES "Visual Studio")
			target_compile_options(${target} PRIVATE /MP)
		endif()
		target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
	else()
		target_compile_options(${target} PRIVATE
			-Wall -Wextra -Wno-missing-field-initializers
			$<$<BOOL:${STRATA_WARNINGS_AS_ERRORS}>:-Werror>)
	endif()

	set_target_properties(${target} PROPERTIES FOLDER "Strata")
endfunction()

# strata_copy_runtime_resources(<target> <source dir> <destination subdir>)
#   Copies a resource directory next to the target's binary after every build.
function(strata_copy_runtime_resources target sourceDir destinationSubdir)
	add_custom_command(TARGET ${target} POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			"${sourceDir}" "$<TARGET_FILE_DIR:${target}>/${destinationSubdir}"
		COMMENT "Copying ${destinationSubdir} for ${target}"
		VERBATIM)
endfunction()
