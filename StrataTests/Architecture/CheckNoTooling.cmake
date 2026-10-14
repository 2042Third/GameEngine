# Fails when the linker map of a game's executable (StrataRuntime, linked with a map file next to it) names an object or a
# library of tooling: Dear ImGui and the engine's ImGui layer, ImGuizmo, the asset pipeline, the network layer or zstd's
# compressor. The proof, beside the configure-time check of what StrataRuntime links (strata_check_link_closure), that no
# game contains them. Map formats differ (MSVC /MAP, GNU ld -Map, Apple ld -map), so names are matched as object and
# library basenames wherever they appear.
#
#   cmake -DMAP=<file.map> -P CheckNoTooling.cmake

if(NOT MAP)
	message(FATAL_ERROR "CheckNoTooling: MAP is required")
endif()
if(NOT EXISTS "${MAP}")
	message(FATAL_ERROR "CheckNoTooling: the linker map '${MAP}' does not exist: StrataRuntime is linked with it")
endif()

# Object basenames (without .cpp.o, .c.o, .o or .obj).
set(forbiddenObjects
	# Dear ImGui and the engine's ImGui layer
	imgui imgui_demo imgui_draw imgui_tables imgui_widgets imgui_stdlib ImGuiLayer ImGuiPlatform ImGuiRenderer ImGuiModule
	ImGuizmo
	# The asset pipeline
	EditorAssetManager AssetImporter AssetImporters GltfImporter TextureImporter TextureCooker BC7Encoder AssetPackWriter
	AssetDependencies
	# The network layer
	RpcServer RpcClient JsonRpc EditorSession WindowsSocket PosixSocket Socket
	# zstd's compressor
	zstd_compress zstd_compress_literals zstd_compress_sequences zstd_compress_superblock zstd_double_fast zstd_fast
	zstd_lazy zstd_ldm zstd_opt zstd_preSplit huf_compress fse_compress hist zdict cover fastcover divsufsort)
# Library basenames (without lib...a or .lib).
set(forbiddenLibraries imgui ImGuizmo nfd cgltf zstd_compress Strata_ImGui Strata_AssetPipeline Strata_Network
	StrataEditorCore StrataEditorUI StrataCLILib)

# One pass per kind over the map's lines (maps reach hundreds of megabytes: the lines are filtered as they are read).
string(REPLACE ";" "|" objectNames "${forbiddenObjects}")
string(REPLACE ";" "|" libraryNames "${forbiddenLibraries}")
# "dir/Name.cpp.o", "lib.a(Name.cpp.o)", "Name.obj", "lib:Name.obj"
file(STRINGS "${MAP}" objectLines REGEX "(^|[/\\\\(: \t])(${objectNames})(\\.(c|cpp|m|mm))?\\.(o|obj)([^A-Za-z0-9_]|$)")
# "dir/libName.a", "Name.lib"
file(STRINGS "${MAP}" libraryLines REGEX "(^|[/\\\\(: \t])(lib)?(${libraryNames})\\.(a|lib)([^A-Za-z0-9_]|$)")

set(found ${objectLines} ${libraryLines})
list(REMOVE_DUPLICATES found)
list(LENGTH found count)
if(count GREATER 0)
	list(SUBLIST found 0 20 shown)
	string(REPLACE ";" "\n  " shown "${shown}")
	message(FATAL_ERROR "The game executable contains tooling (${MAP}), ${count} lines name it, among them:\n  ${shown}")
endif()
message(STATUS "No tooling in ${MAP}")
