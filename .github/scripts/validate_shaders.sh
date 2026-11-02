#!/usr/bin/env bash
# Validates every shader module the engine embeds (the final modules StrataShaderTool wrote, listed in the build's
# ShaderEntries.txt) with spirv-val in the Vulkan 1.2 environment. The compiler validates what it writes, but the tool
# patches some modules afterwards (Docs/DeveloperGuide.md, "Shaders").
#
# validate_shaders.sh <the build's Strata/EmbeddedShaders folder>
# A failure prints "<source file>:1: error: <name>: spirv-val: <first lines>", which annotate_failures.py places on the
# shader's file. Exit 1 unless there are entries and every one is valid.
set -u

if [ $# -ne 1 ]; then
	echo "usage: validate_shaders.sh <EmbeddedShaders folder>" >&2
	exit 2
fi
dir="$1"
manifest="$dir/ShaderEntries.txt"
if [ ! -f "$manifest" ]; then
	echo "$manifest:1: error: the shader manifest is missing (configure and build first)"
	exit 1
fi
if ! version=$(spirv-val --version 2>&1); then
	echo "error: spirv-val does not run: $version"
	exit 1
fi
printf '%s\n' "$version" | head -1

total=0
valid=0
while IFS='|' read -r name source stage defines; do
	[ -n "$name" ] || continue
	total=$((total + 1))
	if output=$(spirv-val --target-env vulkan1.2 "$dir/$name.spv" 2>&1); then
		valid=$((valid + 1))
	else
		first=$(printf '%s\n' "$output" | head -3 | tr '\n' ' ')
		echo "$source:1: error: $name: spirv-val: $first"
	fi
done < "$manifest"

echo "$valid of $total embedded shaders are valid SPIR-V for Vulkan 1.2"
[ "$total" -gt 0 ] && [ "$valid" -eq "$total" ]
