# strata_json_escape(<out> <text>): <text> made safe to paste between the quotes of a JSON string (backslashes, quotes and
# the control characters CMake strings can hold are escaped), e.g. a path written into an editor command script.

function(strata_json_escape out text)
	string(REPLACE "\\" "\\\\" text "${text}")
	string(REPLACE "\"" "\\\"" text "${text}")
	string(REPLACE "\n" "\\n" text "${text}")
	string(REPLACE "\r" "\\r" text "${text}")
	string(REPLACE "\t" "\\t" text "${text}")
	set(${out} "${text}" PARENT_SCOPE)
endfunction()
