#!/usr/bin/env bash
# Converts every shader module the engine embeds (the build's ShaderEntries.txt) to Metal with MoltenVK's own converter
# (Homebrew's molten-vk; spirv-cross only if it is missing) and compiles the result with Apple's Metal compiler. No Mac
# with a GPU runs CI, so this is the lower bound of what MoltenVK does at run time: it configures argument buffers and
# resource indices from the pipeline layout, which an offline conversion cannot know (Docs/DeveloperGuide.md, "Shaders").
#
# convert_shaders_to_metal.sh <the build's Strata/EmbeddedShaders folder> <output folder> [--report-only]
# A failure prints "<source file>:1: error: <name>: <converter|metal>: <first lines>". Exit 1 on a failure not in
# c_KnownFailures, or when a listed entry now converts and compiles (the list only shrinks); --report-only prints and
# never fails.
set -u

# The entries the offline conversion refuses whatever compiled them, each with the first error line as its reason. Filled
# from the first run of this step, on the modules of the compiler before Slang: they read the bindless table (set 1, Texture2D u_Textures[]), a
# runtime array MoltenVK places in an argument buffer it configures from the pipeline layout at run time, which the
# offline converter cannot know. Reason of each: "[mvk-error] SPIR-V to MSL conversion error: Runtime sized variables must
# be in device storage argument buffers."
c_KnownFailures=(
	GameUI/UIQuad.frag
	Scene/DepthPrepass.frag
	Scene/DepthPrepassNoEntityIDs.frag
	Scene/DepthPrepassNoEntityIDsSurface.frag
	Scene/DepthPrepassNoEntityIDsVelocity.frag
	Scene/DepthPrepassSurface.frag
	Scene/DepthPrepassVelocity.frag
	Scene/Forward.frag
	Scene/ForwardGlass.frag
	Scene/ForwardGlassNoEntityIDs.frag
	Scene/ForwardGlassNoEntityIDsVelocity.frag
	Scene/ForwardGlassVelocity.frag
	Scene/ForwardTransparent.frag
	Scene/SelectionMask.frag
	Scene/ShadowMask.frag
)

if [ $# -lt 2 ] || [ $# -gt 3 ] || { [ $# -eq 3 ] && [ "$3" != "--report-only" ]; }; then
	echo "usage: convert_shaders_to_metal.sh <EmbeddedShaders folder> <output folder> [--report-only]" >&2
	exit 2
fi
dir="$1"
out="$2"
reportOnly=0
[ $# -eq 3 ] && reportOnly=1
manifest="$dir/ShaderEntries.txt"
if [ ! -f "$manifest" ]; then
	echo "$manifest:1: error: the shader manifest is missing (configure and build first)"
	exit 1
fi
mkdir -p "$out"

converter=MoltenVKShaderConverter
if ! command -v MoltenVKShaderConverter >/dev/null 2>&1; then
	echo "MoltenVKShaderConverter is not on PATH: converting with spirv-cross"
	if ! command -v spirv-cross >/dev/null 2>&1; then
		brew install spirv-cross || { echo "error: neither MoltenVKShaderConverter nor spirv-cross is available"; exit 1; }
	fi
	converter=spirv-cross
fi

# Images with Xcode 26 install the Metal toolchain as a separate component.
if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
	echo "The Metal compiler does not run: downloading the Metal toolchain"
	xcodebuild -downloadComponent MetalToolchain
	if ! xcrun -sdk macosx metal --version >/dev/null 2>&1; then
		echo "error: the Metal compiler (xcrun -sdk macosx metal) does not run, also after downloading the Metal toolchain"
		exit 1
	fi
fi
xcrun -sdk macosx metal --version 2>&1 | head -1

total=0
passed=0
failedNames=()
passedNames=()
while IFS='|' read -r name source stage defines; do
	[ -n "$name" ] || continue
	total=$((total + 1))
	id=$(printf '%s' "$name" | sed 's/[^A-Za-z0-9_]/_/g')
	module="$dir/$name.spv"
	metal="$out/$id.metal"
	rm -f "$metal" "$out/$id.air"
	if [ "$converter" = "MoltenVKShaderConverter" ]; then
		output=$(MoltenVKShaderConverter -si "$module" -mo "$metal" -mv 3.0 -mp macos -mab -q 2>&1)
		result=$?
	else
		case "$stage" in
			vertex) short=vert ;;
			fragment) short=frag ;;
			compute) short=comp ;;
			*) short="$stage" ;;
		esac
		output=$(spirv-cross --msl --msl-version 30000 --msl-argument-buffers --msl-argument-buffer-tier 1 --entry main \
			--stage "$short" --output "$metal" "$module" 2>&1)
		result=$?
	fi
	if [ "$result" -ne 0 ] || [ ! -s "$metal" ]; then
		echo "$source:1: error: $name: converter: $(printf '%s\n' "$output" | head -3 | tr '\n' ' ')"
		failedNames+=("$name")
		continue
	fi
	if ! output=$(xcrun -sdk macosx metal -std=metal3.0 -c "$metal" -o "$out/$id.air" 2>&1); then
		echo "$source:1: error: $name: metal: $(printf '%s\n' "$output" | grep -m 3 -E 'error|warning' | tr '\n' ' ')"
		failedNames+=("$name")
		continue
	fi
	passed=$((passed + 1))
	passedNames+=("$name")
done < "$manifest"

echo "$passed of $total embedded shaders convert to Metal ($converter) and compile (Metal 3.0)"

problems=0
for name in "${failedNames[@]+"${failedNames[@]}"}"; do
	known=0
	for entry in "${c_KnownFailures[@]+"${c_KnownFailures[@]}"}"; do
		[ "$entry" = "$name" ] && known=1
	done
	if [ "$known" -eq 0 ]; then
		echo "error: $name does not convert or compile, and c_KnownFailures does not list it"
		problems=$((problems + 1))
	fi
done
for name in "${passedNames[@]+"${passedNames[@]}"}"; do
	for entry in "${c_KnownFailures[@]+"${c_KnownFailures[@]}"}"; do
		if [ "$entry" = "$name" ]; then
			echo "error: $name now converts and compiles: remove it from c_KnownFailures"
			problems=$((problems + 1))
		fi
	done
done
if [ "$reportOnly" -eq 1 ]; then
	echo "Report only: $problems problems not counted"
	exit 0
fi
[ "$total" -gt 0 ] && [ "$problems" -eq 0 ]
