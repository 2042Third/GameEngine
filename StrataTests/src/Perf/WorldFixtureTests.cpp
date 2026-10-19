#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/WorldFixture.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Process.h"
#include "Strata/Project/Project.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Renderer/RendererComponents.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
#endif

#include <stb_image.h>

#include <set>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Three districts, the last one partial, and static bodies: every case of the generator, small enough for Debug.
	const WorldSceneSpec c_SmallWorld { "Small", 2 * c_WorldDistrictCubes + 1000, true };

	std::string ReadFile(const std::filesystem::path& path)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		REQUIRE(text.has_value());
		return *text;
	}

}

TEST_SUITE("PerfLab.WorldFixture")
{
	TEST_CASE("The audit's worlds have the entity counts of its runs")
	{
		CHECK(GetWorldEntityCount(c_World250k) == 250104);
		CHECK(GetWorldEntityCount(c_World1M) == 1000404);
		CHECK(GetWorldDistrictCount(c_WorldDistrictCubes) == 1);
		CHECK(GetWorldDistrictCount(c_WorldDistrictCubes + 1) == 2);
		CHECK(GetWorldEntityCount(c_SmallWorld) == c_WorldFixedEntities + 3 + c_SmallWorld.Cubes);
		// IDs differ per scene name and per entity, and are never null.
		CHECK(GetWorldEntityID(c_SmallWorld, 0).IsValid());
		CHECK(GetWorldEntityID(c_SmallWorld, 0) != GetWorldEntityID(c_SmallWorld, 1));
		CHECK(GetWorldEntityID(c_SmallWorld, 0) != GetWorldEntityID(c_World1M, 0));
	}

	TEST_CASE("A world scene's file holds exactly what building the world in a scene and saving it compact gives")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("WorldFixture");
		WorldProjectSpec spec;
		spec.Scenes = { c_SmallWorld };
		std::string error;
		const std::optional<WorldProject> project = WriteWorldProject(directory / "World", spec, error);
		INFO(error);
		REQUIRE(project.has_value());
		REQUIRE(project->Scenes.size() == 1);
		REQUIRE(project->Materials.size() == spec.Materials);
		const WorldSceneFile& file = project->Scenes[0];
		CHECK(file.AssetPath == "Scenes/Small.stscene");
		CHECK(file.Entities == GetWorldEntityCount(c_SmallWorld));
		CHECK(file.Bytes == FileSystem::GetFileSize(file.Path).value_or(0));

		// The project: its start scene and every asset with a .meta file naming its handle.
		const Ref<Project> loaded = Project::Load(project->ProjectFile, &error);
		REQUIRE(loaded);
		CHECK(loaded->GetConfig().StartScene == project->StartScene);
		std::filesystem::path sceneMeta = file.Path;
		sceneMeta += ".meta";
		CHECK(FileSystem::Exists(sceneMeta));
		CHECK(FileSystem::Exists(project->Directory / "Assets" / "Scenes" / "Empty.stscene.meta"));
		CHECK(FileSystem::Exists(project->Directory / "Assets" / "Materials" / "World_7.stmat.meta"));
		const std::optional<nlohmann::json> meta = JsonUtils::Parse(ReadFile(sceneMeta));
		REQUIRE(meta.has_value());
		CHECK(UUIDFromJson((*meta)["Handle"]) == file.Handle);

		// Byte for byte what the engine writes for the same world, compact.
		Scene built(c_SmallWorld.Name);
		REQUIRE(BuildWorldScene(built, c_SmallWorld, project->Materials, error));
		CHECK(built.GetEntityCount() == file.Entities);
		const std::string text = ReadFile(file.Path);
		CHECK(text == JsonUtils::Dump(SceneSerializer::Serialize(built)) + "\n");

		// Loaded, the world has its districts, cubes, materials and bodies where the spec says.
		const Ref<Scene> scene = SceneSerializer::LoadFromFile(file.Path, &error);
		REQUIRE(scene);
		CHECK(scene->GetEntityCount() == file.Entities);
		const std::vector<UUID>& roots = scene->GetRootEntities();
		REQUIRE(roots.size() == c_WorldFixedEntities + 3);
		CHECK(scene->GetEntityByUUID(roots[0]).HasComponent<CameraComponent>());
		CHECK(scene->GetEntityByUUID(roots[0]).GetComponent<TransformComponent>().Translation.y == doctest::Approx(c_WorldCameraHeight));
		CHECK(scene->GetEntityByUUID(roots[1]).HasComponent<DirectionalLightComponent>());
		CHECK(scene->GetEntityByUUID(roots[2]).GetComponent<SkyLightComponent>().Source == SkyLightSource::Procedural);
		CHECK(scene->GetEntityByUUID(roots[3]).GetComponent<MeshRendererComponent>().Mesh == BuiltinAssets::PlaneMesh);
		const size_t expectedChildren[] = { c_WorldDistrictCubes, c_WorldDistrictCubes, 1000 };
		size_t bodies = 0;
		std::set<AssetHandle> materials;
		for (size_t district = 0; district < 3; district++)
		{
			const Entity districtEntity = scene->GetEntityByUUID(roots[c_WorldFixedEntities + district]);
			const std::vector<Entity> cubes = districtEntity.GetChildren();
			CHECK(cubes.size() == expectedChildren[district]);
			for (const Entity cube : cubes)
			{
				CHECK(cube.GetComponent<MeshRendererComponent>().Mesh == BuiltinAssets::CubeMesh);
				materials.insert(cube.GetComponent<MeshRendererComponent>().Material);
#if ST_WITH_PHYSICS
				if (cube.HasComponent<RigidBodyComponent>())
				{
					bodies++;
					CHECK(cube.GetComponent<RigidBodyComponent>().Type == RigidBodyType::Static);
					CHECK(cube.HasComponent<BoxColliderComponent>());
				}
#endif
			}
		}
#if ST_WITH_PHYSICS
		CHECK(bodies == (c_SmallWorld.Cubes + c_WorldColliderInterval - 1) / c_WorldColliderInterval);
#else
		CHECK(bodies == 0); // Without the physics module the world has no colliders
#endif
		CHECK(materials == std::set<AssetHandle>(project->Materials.begin(), project->Materials.end()));
		// Cubes stand c_WorldCubeSpacing apart.
		const std::vector<Entity> firstCubes = scene->GetEntityByUUID(roots[c_WorldFixedEntities]).GetChildren();
		CHECK(firstCubes[1].GetComponent<TransformComponent>().Translation.x - firstCubes[0].GetComponent<TransformComponent>().Translation.x
			== doctest::Approx(c_WorldCubeSpacing));
	}

	TEST_CASE("World files are the same on every run, whatever the thread count")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("WorldFixtureRuns");
		WorldProjectSpec spec;
		spec.Scenes = { c_SmallWorld };
		spec.Materials = 3;
		std::string error;
		std::optional<WorldProject> serial;
		{
			// Without the job system, every entity is generated on this thread.
			serial = WriteWorldProject(directory / "Inline", spec, error);
			INFO(error);
			REQUIRE(serial.has_value());
		}
		std::optional<WorldProject> parallel;
		{
			ScopedJobSystem jobs(4, 1);
			parallel = WriteWorldProject(directory / "Parallel", spec, error);
			INFO(error);
			REQUIRE(parallel.has_value());
		}
		CHECK(ReadFile(serial->Scenes[0].Path) == ReadFile(parallel->Scenes[0].Path));
		CHECK(serial->Materials == parallel->Materials);
		CHECK(serial->Scenes[0].Handle == parallel->Scenes[0].Handle);

		// A directory that holds a project already is refused.
		CHECK_FALSE(WriteWorldProject(directory / "Inline", spec, error).has_value());
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("Many-assets projects hold their materials in folders and a scene that uses each once")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ManyAssets");
		constexpr uint32_t c_Materials = 2 * c_ManyAssetsFolderSize + 5;
		std::string error;
		const std::optional<ManyAssetsProject> project = WriteManyAssetsProject(directory / "Many", c_Materials, error);
		INFO(error);
		REQUIRE(project.has_value());
		REQUIRE(project->Materials.size() == c_Materials);
		const std::filesystem::path materials = project->Directory / "Assets" / "Materials";
		CHECK(FileSystem::Exists(materials / "Folder_00" / "Material_00000.stmat.meta"));
		CHECK(FileSystem::Exists(materials / "Folder_02" / "Material_02004.stmat"));
		CHECK_FALSE(FileSystem::Exists(materials / "Folder_03"));

		const Ref<Project> loaded = Project::Load(project->ProjectFile, &error);
		REQUIRE(loaded);
		CHECK(loaded->GetConfig().StartScene == project->Scene);
		const Ref<Scene> scene = SceneSerializer::LoadFromFile(project->Directory / "Assets" / "Scenes" / "Many.stscene", &error);
		REQUIRE(scene);
		CHECK(scene->GetEntityCount() == c_Materials + 2);
		std::multiset<AssetHandle> used;
		for (const auto [handle, renderer] : scene->GetRegistry().view<MeshRendererComponent>().each())
			used.insert(renderer.Material);
		CHECK(used == std::multiset<AssetHandle>(project->Materials.begin(), project->Materials.end()));

		// Without .meta files, as when files are copied into a project.
		const std::optional<ManyAssetsProject> bare = WriteManyAssetsProject(directory / "Bare", 3, error, false);
		REQUIRE(bare.has_value());
		const std::filesystem::path bareMaterials = bare->Directory / "Assets" / "Materials" / "Folder_00";
		CHECK(FileSystem::Exists(bareMaterials / "Material_00002.stmat"));
		CHECK_FALSE(FileSystem::Exists(bareMaterials / "Material_00002.stmat.meta"));
	}

	TEST_CASE("Texture folders hold decodable RGB noise of the size asked for")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("TextureFolder");
		std::string error;
		const std::optional<std::vector<std::filesystem::path>> files = WriteTextureFolder(directory / "Textures", 3, 32, 5, error);
		INFO(error);
		REQUIRE(files.has_value());
		REQUIRE(files->size() == 3);
		CHECK((*files)[2].filename() == "Texture_0002.png");
		std::vector<std::vector<uint8_t>> decoded;
		for (const std::filesystem::path& file : *files)
		{
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(file);
			REQUIRE(bytes.has_value());
			int width = 0;
			int height = 0;
			int channels = 0;
			stbi_uc* pixels = stbi_load_from_memory(bytes->data(), static_cast<int>(bytes->size()), &width, &height, &channels, 3);
			REQUIRE(pixels != nullptr);
			CHECK(width == 32);
			CHECK(height == 32);
			CHECK(channels == 3);
			decoded.emplace_back(pixels, pixels + 32 * 32 * 3);
			stbi_image_free(pixels);
		}
		// Each texture is a noise of its own.
		CHECK(decoded[0] != decoded[1]);
		// The registration gate's folder.
		const std::optional<std::vector<std::filesystem::path>> small = WriteSmallTextureFolder(directory / "Small", error);
		REQUIRE(small.has_value());
		CHECK(small->size() == c_SmallTextureFolderCount);
		CHECK_FALSE(WriteTextureFolder(directory / "None", 1, 0, 1, error).has_value());
	}

	TEST_CASE("The fixture helper modes write the projects for manual runs")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("WorldFixtureHelper");
		auto runHelper = [](std::vector<std::string> arguments)
		{
			ProcessSpecification specification;
			specification.Executable = GetTestExecutablePath();
			specification.Arguments = std::move(arguments);
			return Process::Run(specification, std::chrono::minutes(2));
		};
		const Process::RunResult world = runHelper({ "--strata-test-helper=write-world-project", FileSystem::ToUTF8(directory / "World"), "Tiny:30:colliders",
			"Plain:10" });
		INFO(world.Output);
		CHECK(world.ExitCode == 0);
		CHECK(FileSystem::Exists(directory / "World" / "Assets" / "Scenes" / "Tiny.stscene"));
		CHECK(FileSystem::Exists(directory / "World" / "Assets" / "Scenes" / "Plain.stscene"));

		CHECK(runHelper({ "--strata-test-helper=write-many-assets-project", FileSystem::ToUTF8(directory / "Many"), "4" }).ExitCode == 0);
		CHECK(FileSystem::Exists(directory / "Many" / "Assets" / "Materials" / "Folder_00" / "Material_00003.stmat"));
		CHECK(runHelper({ "--strata-test-helper=write-texture-folder", FileSystem::ToUTF8(directory / "Textures"), "2", "8" }).ExitCode == 0);
		CHECK(FileSystem::Exists(directory / "Textures" / "Texture_0001.png"));

		// Wrong arguments.
		CHECK(runHelper({ "--strata-test-helper=write-world-project", FileSystem::ToUTF8(directory / "Bad"), "NoCount" }).ExitCode == 2);
		CHECK(runHelper({ "--strata-test-helper=write-texture-folder", FileSystem::ToUTF8(directory / "Bad") }).ExitCode == 2);
	}
}
