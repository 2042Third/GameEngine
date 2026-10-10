#include <doctest/doctest.h>

#include "Perf/StressProject.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <glm/gtc/matrix_access.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Two small textures, one large, five materials: districts in a 3 x 2 grid, the sixth cell repeating material 0.
	StressProjectSpec GetTinySpec()
	{
		StressProjectSpec spec;
		spec.Textures2k = 2;
		spec.Textures4k = 1;
		spec.Materials = 5;
		spec.Entities = 36;
		spec.GridResolution = 4;
		spec.SmallTextureSize = 16;
		spec.LargeTextureSize = 32;
		return spec;
	}

}

TEST_SUITE("Asset.StressProject")
{
	TEST_CASE("The stress project imports cleanly and groups its materials by district")
	{
		const StressProjectSpec spec = GetTinySpec();
		const std::filesystem::path directory = CreateTemporaryDirectory("StressProject");
		std::string error;
		const std::optional<StressProject> project = WriteStressProject(directory / "Stress", spec, error);
		REQUIRE_MESSAGE(project, error);
		CHECK(project->Textures.size() == 3);
		CHECK(project->Materials.size() == 5);
		CHECK(project->TextureBytes == 2 * GetStressTextureBytes(16) + GetStressTextureBytes(32));
		CHECK(project->LargestTextureBytes == GetStressTextureBytes(32));
		CHECK(GetStressTextureBytes(2048) == 22369620);
		CHECK(FileSystem::IsRegularFile(directory / "Stress" / "Stress.stproj"));

		// Writing into a directory that already holds a project fails.
		std::string secondError;
		CHECK_FALSE(WriteStressProject(directory / "Stress", spec, secondError));
		CHECK_FALSE(secondError.empty());

		REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Stress.stpak", error), error);
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(directory / "Stress.stpak", &error);
		REQUIRE_MESSAGE(manager, error);
		for (AssetHandle texture : project->Textures)
			CHECK(manager->GetAssetType(texture) == AssetType::Texture);
		for (AssetHandle material : project->Materials)
			CHECK(manager->GetAssetType(material) == AssetType::Material);
		CHECK(manager->GetAssetType(project->GridModel) == AssetType::Model);
		CHECK(manager->GetAssetType(project->GridMesh) == AssetType::Mesh);

		const Ref<Asset> sceneAsset = manager->LoadAssetSync(project->WorldScene);
		REQUIRE(sceneAsset);
		REQUIRE(sceneAsset->GetType() == AssetType::Scene);
		const Ref<Scene> scene = std::static_pointer_cast<SceneAsset>(sceneAsset)->CreateScene(&error);
		REQUIRE_MESSAGE(scene, error);

		// Every object takes the material of its district: cells of 2000/3 x 2000/2 units, numbered row by row.
		uint32_t objects = 0;
		uint32_t terrains = 0;
		std::set<uint64_t> materialsUsed;
		for (auto [entity, renderer, transform] : scene->GetRegistry().view<MeshRendererComponent, TransformComponent>().each())
		{
			if (renderer.Mesh == project->GridMesh)
			{
				terrains++;
				continue;
			}
			objects++;
			const uint32_t column = static_cast<uint32_t>((transform.Translation.x + c_StressWorldSize * 0.5f) / (c_StressWorldSize / 3.0f));
			const uint32_t row = static_cast<uint32_t>((transform.Translation.z + c_StressWorldSize * 0.5f) / (c_StressWorldSize / 2.0f));
			CAPTURE(transform.Translation.x);
			CAPTURE(transform.Translation.z);
			CHECK(renderer.Material == project->Materials[(row * 3 + column) % 5]);
			materialsUsed.insert(static_cast<uint64_t>(renderer.Material));
		}
		CHECK(objects == spec.Entities);
		CHECK(terrains == 1);
		CHECK(materialsUsed.size() == 5);
	}

	TEST_CASE("The stress sweep holds at its stops and moves between them")
	{
		const StressProjectSpec spec = GetTinySpec();
		StressSweepPath path;
		path.Stops = 3;
		path.MoveFrames = 10;
		path.HoldFrames = 20;
		REQUIRE(path.GetFrameCount() == 90);
		const auto position = [&](uint32_t frame)
		{
			return GetStressSweepFrame(spec, path, frame, 16.0f / 9.0f).Camera.Position;
		};

		// The first segment holds at the first stop throughout.
		const StressSweepFrame first = GetStressSweepFrame(spec, path, 0, 16.0f / 9.0f);
		CHECK(first.Holding);
		CHECK(first.Stop == 0);
		CHECK(first.FramesHolding == 0);
		CHECK(GetStressSweepFrame(spec, path, 29, 16.0f / 9.0f).FramesHolding == 29);
		CHECK(position(29) == position(0));

		// Then the camera leaves it, arrives at the next stop at rest and holds there.
		const StressSweepFrame leaving = GetStressSweepFrame(spec, path, 30, 16.0f / 9.0f);
		CHECK_FALSE(leaving.Holding);
		CHECK(leaving.Stop == 1);
		CHECK(leaving.Camera.Position == position(0));
		CHECK(position(35) != position(0));
		const StressSweepFrame arrived = GetStressSweepFrame(spec, path, 40, 16.0f / 9.0f);
		CHECK(arrived.Holding);
		CHECK(arrived.Stop == 1);
		CHECK(arrived.FramesHolding == 0);
		CHECK(position(59) == arrived.Camera.Position);
		CHECK(arrived.Camera.Position != position(0));

		// The stops lie in the first and the last district of a serpentine walk over the 3 x 2 grid: the first cell, then
		// the last one (row 1 runs backwards, so its last cell is column 0).
		const float cellWidth = c_StressWorldSize / 3.0f;
		CHECK(position(0).x < -c_StressWorldSize * 0.5f + cellWidth);
		CHECK(position(89).x < -c_StressWorldSize * 0.5f + cellWidth);
		CHECK(position(89).z > position(0).z);
		// Past the end, the camera holds at the last stop.
		CHECK(GetStressSweepFrame(spec, path, 500, 16.0f / 9.0f).Stop == 2);
		CHECK(position(500) == position(89));

		// The camera looks down at the stop from the configured height.
		const SceneCamera& camera = arrived.Camera;
		const glm::vec3 forward = -glm::vec3(glm::row(camera.View, 2));
		CHECK(forward.y < -0.8f);
		CHECK(camera.Far == doctest::Approx(path.FarClip));
	}

	TEST_CASE("Sweep settings and results survive the trip through JSON")
	{
		StreamingSweepSettings settings;
		settings.Pack = CreateTemporaryDirectory("StressJson") / "Stress.stpak";
		settings.Scene = UUID(0x5A00000000004000ull);
		settings.Spec = GetTinySpec();
		settings.Path.Stops = 3;
		settings.Path.FarClip = 750.0f;
		settings.TextureBudget = 123456789;
		settings.Width = 640;
		settings.FrameRate = 30;
		std::string error;
		const std::optional<StreamingSweepSettings> settingsBack = StreamingSweepSettingsFromJson(ToJson(settings), error);
		REQUIRE_MESSAGE(settingsBack, error);
		CHECK(settingsBack->Pack == settings.Pack);
		CHECK(settingsBack->Scene == settings.Scene);
		CHECK(settingsBack->Spec.Materials == 5);
		CHECK(settingsBack->Spec.LargeTextureSize == 32);
		CHECK(settingsBack->Path.Stops == 3);
		CHECK(settingsBack->Path.FarClip == 750.0f);
		CHECK(settingsBack->TextureBudget == 123456789);
		CHECK(settingsBack->Width == 640);
		CHECK(settingsBack->FrameRate == 30);

		StreamingSweepResult result;
		result.Completed = true;
		result.Frames = 3;
		result.MaxResidentTextureBytes = 5000000000ull;
		result.MaxFinalizeMs = 2.5f;
		result.FramesToSettle = { 0, 7, c_StopNeverSettled };
		result.PeakPrivateBytes = 700000000;
		result.PeakWorkingSetBytes = 300000000;
		result.ResidentTextureBytes = { 1, 2, 3 };
		result.PendingAssets = { 4, 0, 0 };
		result.FinalizeMs = { 0.5f, 2.5f, 0.0f };
		result.PrivateBytes = { 10, 20, 30 };
		result.DeviceBytes = { 5, 6, 7 };
		const std::optional<StreamingSweepResult> resultBack = StreamingSweepResultFromJson(ToJson(result), error);
		REQUIRE_MESSAGE(resultBack, error);
		CHECK(resultBack->Completed);
		CHECK(resultBack->MaxResidentTextureBytes == 5000000000ull);
		CHECK(resultBack->MaxFinalizeMs == 2.5f);
		CHECK(resultBack->FramesToSettle == result.FramesToSettle);
		CHECK(resultBack->PendingAssets == result.PendingAssets);
		CHECK(resultBack->FinalizeMs == result.FinalizeMs);
		CHECK(resultBack->PrivateBytes == result.PrivateBytes);
		CHECK(resultBack->DeviceBytes == result.DeviceBytes);
		CHECK(resultBack->PeakWorkingSetBytes == 300000000);

		// Missing or mistyped members are rejected with a reason.
		nlohmann::json broken = ToJson(result);
		broken["FramesToSettle"] = "seven";
		error.clear();
		CHECK_FALSE(StreamingSweepResultFromJson(broken, error));
		CHECK_FALSE(error.empty());
		nlohmann::json noSpec = ToJson(settings);
		noSpec.erase("Spec");
		error.clear();
		CHECK_FALSE(StreamingSweepSettingsFromJson(noSpec, error));
		CHECK_FALSE(error.empty());
	}
}
