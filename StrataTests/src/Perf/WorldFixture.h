#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{
	class Scene;
}

// Realistic worlds and projects with many assets for the world-scale gates: the ports of the scale audit's generators
// (gen_world.py, gen_many.py, gen_textures.py). Everything is deterministic (fixed seeds, handles and entity IDs), so every
// run and every machine writes the same bytes. Files go only where the caller says, which for tests is a directory of
// Tests::CreateTemporaryDirectory(). world-io's gates use it, and world-partition's and gpu-driven-rendering's reuse it:
// this API is only ever added to, never changed.
//
// A world scene: a camera at walking height near the centre, a sun, a procedural sky, a ground plane under the whole world,
// and `Cubes` unit cubes (the built-in cube mesh, each with one of the project's materials) on a square grid
// c_WorldCubeSpacing apart, grouped under district entities of c_WorldDistrictCubes (c_WorldDistrictSide on a side), in
// hierarchy order (each district followed by its cubes). With colliders, every c_WorldColliderInterval-th cube is also a
// static body (a RigidBody of type Static and a BoxCollider).
namespace Strata::Tests
{

	constexpr uint32_t c_WorldDistrictSide = 50;
	constexpr uint32_t c_WorldDistrictCubes = c_WorldDistrictSide * c_WorldDistrictSide;
	constexpr float c_WorldCubeSpacing = 4.0f;
	constexpr uint32_t c_WorldColliderInterval = 10;
	// The camera, the sun, the sky and the ground: the entities of a world scene besides its districts and cubes, first in
	// hierarchy order.
	constexpr uint32_t c_WorldFixedEntities = 4;
	constexpr float c_WorldCameraHeight = 1.7f;

	struct WorldSceneSpec
	{
		std::string Name; // The scene's name and its file's (Scenes/<Name>.stscene); it also seeds the entity IDs
		uint32_t Cubes = 0;
		bool Colliders = false;
	};

	// The audit's realistic worlds: 250,104 and 1,000,404 entities.
	inline const WorldSceneSpec c_World250k { "World250k", 250000, true };
	inline const WorldSceneSpec c_World1M { "World1M", 1000000, true };

	// Districts of a world scene of `cubes` cubes (the last one may hold fewer than c_WorldDistrictCubes).
	uint32_t GetWorldDistrictCount(uint32_t cubes);
	// Every entity of the world scene: its cubes, its districts and the c_WorldFixedEntities.
	uint32_t GetWorldEntityCount(const WorldSceneSpec& spec);
	// The ID of the world scene's entity at `index` in hierarchy order (the camera is 0).
	UUID GetWorldEntityID(const WorldSceneSpec& spec, uint32_t index);

	struct WorldProjectSpec
	{
		std::vector<WorldSceneSpec> Scenes;
		uint32_t Materials = 8; // Materials/World_<n>.stmat, each a color of its own; cube i has material i modulo the count
	};

	struct WorldSceneFile
	{
		std::string Name;
		AssetHandle Handle = UUID::Null();
		std::string AssetPath;      // Relative to the asset directory ("Scenes/World1M.stscene"), as commands take it
		std::filesystem::path Path; // Absolute
		uint32_t Entities = 0;
		uint64_t Bytes = 0;         // The file's size
	};

	// What WriteWorldProject wrote. Handles are fixed.
	struct WorldProject
	{
		std::filesystem::path Directory;   // Holds World.stproj and Assets/
		std::filesystem::path ProjectFile;
		// The project's start scene, Scenes/Empty.stscene (a camera and a sun): opening the project loads nothing large.
		AssetHandle StartScene = UUID::Null();
		std::vector<WorldSceneFile> Scenes;  // In the spec's order
		std::vector<AssetHandle> Materials;  // In the order the cubes use them
	};

	// Writes a project with the spec's world scenes into `directory` (created if needed; it must not hold a project yet): the
	// materials, the empty start scene and each world scene, every asset with its .meta, and the project file. Nothing is
	// imported. World scenes are written compact, on one line, as tools write them (JsonUtils::Dump of
	// SceneSerializer::Serialize: the entities come before the "Strata" header), generated an entity at a time so that a
	// million entities never exist in memory at once. nullopt with the reason in `error` on failure.
	std::optional<WorldProject> WriteWorldProject(const std::filesystem::path& directory, const WorldProjectSpec& spec, std::string& error);

	// Creates the world scene's entities in `scene` (which should be empty), exactly as loading its file creates them: the
	// same IDs, names, components and hierarchy. `materials` are WorldProject::Materials (empty: cubes without a material).
	// For gates that need the world in memory rather than in a file. False with the reason in `error` when the scene cannot
	// hold them.
	bool BuildWorldScene(Scene& scene, const WorldSceneSpec& spec, std::span<const AssetHandle> materials, std::string& error);

	// Materials of a many-assets project per folder (Materials/Folder_<nn>/).
	constexpr uint32_t c_ManyAssetsFolderSize = 1000;

	// What WriteManyAssetsProject wrote. Handles are fixed (with .meta files; without them the editor's first scan gives the
	// materials handles of its own, and the scene's references name handles no asset has).
	struct ManyAssetsProject
	{
		std::filesystem::path Directory;   // Holds Many.stproj and Assets/
		std::filesystem::path ProjectFile;
		AssetHandle Scene = UUID::Null();  // Scenes/Many.stscene, the start scene: one cube per material, roots on a grid
		std::vector<AssetHandle> Materials;
	};

	// Writes a project of `materials` materials, in folders of c_ManyAssetsFolderSize, and a scene that uses every one of
	// them (gen_many.py's projects). Without `withMetaFiles` no material has its .meta file, as when files are copied into a
	// project. nullopt with the reason in `error` on failure.
	std::optional<ManyAssetsProject> WriteManyAssetsProject(const std::filesystem::path& directory, uint32_t materials, std::string& error,
		bool withMetaFiles = true);

	// Writes `count` RGB PNGs of size x size (gradient plus noise, Texture_<nnnn>.png; texture i is seeded with seed + i,
	// seed from 1) into `directory` (created if needed), at compression level 1 as gen_textures.py writes them: a folder to
	// import, not a project. The files' paths in order, or nullopt with the reason in `error`.
	std::optional<std::vector<std::filesystem::path>> WriteTextureFolder(const std::filesystem::path& directory, uint32_t count, uint32_t size,
		uint32_t seed, std::string& error);
	// The registration gate's folder: c_SmallTextureFolderCount textures of c_SmallTextureFolderSize^2.
	constexpr uint32_t c_SmallTextureFolderCount = 600;
	constexpr uint32_t c_SmallTextureFolderSize = 64;
	std::optional<std::vector<std::filesystem::path>> WriteSmallTextureFolder(const std::filesystem::path& directory, std::string& error);

	struct WorldGameOptions
	{
		AssetHandle StartScene = UUID::Null(); // The scene the game starts with; null: the project's start scene
		bool IncludeRuntime = false;           // Copy the runtime executable (tests run the build's own with --game)
		bool CookDocuments = true;             // Cooked scenes in the pack, as exports write them (false: their JSON)
	};

	// What WriteWorldGame exported.
	struct WorldGame
	{
		std::filesystem::path Directory;
		std::filesystem::path Manifest;   // World.stgame: StrataRuntime --game <Manifest>
		std::filesystem::path AssetPack;
		std::filesystem::path Executable; // Empty without the runtime
	};

	// Exports a world project as a game into `outputDirectory` the way the editor does (GameExport, through a headless
	// EditorContext: the project is opened and scanned, the start scene set when the options name one, then project.export),
	// without a GPU. Defined only in builds with the editor (src/Editor/WorldGameExport.cpp). nullopt with the reason in
	// `error` on failure.
	std::optional<WorldGame> WriteWorldGame(const WorldProject& project, const std::filesystem::path& outputDirectory, std::string& error,
		const WorldGameOptions& options = {});

	// The test executable as a helper process that writes the fixtures, for manual runs and screenshots:
	//   --strata-test-helper=write-world-project <directory> <name>:<cubes>[:colliders]...
	//   --strata-test-helper=write-many-assets-project <directory> <materials>
	//   --strata-test-helper=write-texture-folder <directory> <count> <size>
	// Returns the process's exit code (2 for wrong arguments), or nullopt when `mode` is none of these.
	std::optional<int> RunWorldFixtureHelper(std::string_view mode, int argc, char** argv);

}
