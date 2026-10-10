# Copies the sample projects next to the executables, in script mode at build time (the StrataSamples target in
# StrataEditor/CMakeLists.txt), where the editor offers them (project.samples, the launcher's Open Sample):
#
#   cmake -DSOURCE=<repository>/Samples -DDESTINATION=<bin>/Samples -P StrataCopySamples.cmake
#
# The copy holds what the samples' checkout holds: local editor data (.strata, from opening a sample in place) is left
# out. Only changed files are copied, and files no longer in the source are removed, so builds keep the copy in sync
# without rewriting it.

if(NOT SOURCE OR NOT DESTINATION)
	message(FATAL_ERROR "StrataCopySamples.cmake needs -DSOURCE=<samples> and -DDESTINATION=<directory>")
endif()

file(GLOB_RECURSE sourceFiles LIST_DIRECTORIES false RELATIVE "${SOURCE}" "${SOURCE}/*")
list(FILTER sourceFiles EXCLUDE REGEX "(^|/)\\.strata(/|$)")
foreach(file IN LISTS sourceFiles)
	get_filename_component(directory "${DESTINATION}/${file}" DIRECTORY)
	file(MAKE_DIRECTORY "${directory}")
	file(COPY_FILE "${SOURCE}/${file}" "${DESTINATION}/${file}" ONLY_IF_DIFFERENT)
endforeach()

file(GLOB_RECURSE copiedFiles LIST_DIRECTORIES false RELATIVE "${DESTINATION}" "${DESTINATION}/*")
foreach(file IN LISTS copiedFiles)
	if(NOT file IN_LIST sourceFiles)
		file(REMOVE "${DESTINATION}/${file}")
	endif()
endforeach()
