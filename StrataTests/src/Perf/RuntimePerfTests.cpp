#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSerializer.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

// A headless game switching scenes (CTest StrataTests.Perf: Release and Dist): the next scene loads while the running one
// goes on, and the scene a switch replaces is destroyed off the frame, so no frame of a switch may stall the game.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_StartScene = 0x5000;
	constexpr uint64_t c_BigScene = 0x5001;
	constexpr uint64_t c_FirstMaterial = 0x5100;
	constexpr uint32_t c_Materials = 8;
	constexpr auto c_FramePeriod = std::chrono::microseconds(16'667);
	// Frames measured after each switch: the new scene's first frames.
	constexpr uint32_t c_FramesAfterSwitch = 10;

	std::vector<uint8_t> ToBytes(const nlohmann::json& json)
	{
		const std::string text = JsonUtils::Dump(json);
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// The game: a small start scene, and a scene of 100,000 nested entities, every hundredth of which draws the built-in
	// cube with one of eight materials (its dependencies, recorded in the pack as an export records them).
	std::filesystem::path WriteSwitchGame(const std::filesystem::path& directory)
	{
		std::map<uint64_t, std::vector<uint8_t>> data;
		std::vector<AssetPackSource> sources;
		const auto add = [&](uint64_t handle, AssetType type, const std::string& path, std::vector<uint8_t> bytes, std::vector<AssetHandle> dependencies = {})
		{
			AssetPackSource source;
			source.Metadata.Handle = UUID(handle);
			source.Metadata.Type = type;
			source.Metadata.Path = path;
			source.Metadata.Name = path;
			source.Metadata.StoredSize = bytes.size();
			source.Dependencies = std::move(dependencies);
			sources.push_back(std::move(source));
			data[handle] = std::move(bytes);
		};

		Scene start("Start");
		Perf::AddCameraAndLight(start);
		Perf::CreateNestedHierarchy(start, 1000);
		add(c_StartScene, AssetType::Scene, "Scenes/Start.stscene", ToBytes(SceneSerializer::Serialize(start)));

		std::vector<AssetHandle> materials;
		for (uint32_t index = 0; index < c_Materials; index++)
		{
			MaterialProperties properties;
			properties.Roughness = 0.1f * static_cast<float>(index);
			materials.push_back(UUID(c_FirstMaterial + index));
			add(c_FirstMaterial + index, AssetType::Material, "Materials/M" + std::to_string(index) + ".stmat", ToBytes(Material::Create(properties)->Serialize()));
		}
		Scene big("Big");
		Perf::AddCameraAndLight(big);
		const Perf::NestedHierarchy hierarchy = Perf::CreateNestedHierarchy(big, 100'000);
		for (size_t index = 0; index < hierarchy.Entities.size(); index += 100)
		{
			Entity entity = hierarchy.Entities[index];
			MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
			renderer.Mesh = BuiltinAssets::CubeMesh;
			renderer.Material = materials[(index / 100) % materials.size()];
		}
		add(c_BigScene, AssetType::Scene, "Scenes/Big.stscene", ToBytes(SceneSerializer::Serialize(big)), materials);

		std::string error;
		REQUIRE_MESSAGE(AssetPackWriter::Write(directory / "Switch.stpak", sources, [&data](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = data.at(static_cast<uint64_t>(asset.Handle));
			return true;
		}, {}, nullptr, &error), error);
		GameManifest manifest;
		manifest.Name = "Switch";
		manifest.AssetPack = "Switch.stpak";
		manifest.StartScene = UUID(c_StartScene);
		REQUIRE_MESSAGE(manifest.Save(directory / "Switch.stgame", &error), error);
		return directory / "Switch.stgame";
	}

	// Asks the running scene for `target` and runs frames at 60 per second, as headless games do, until c_FramesAfterSwitch
	// frames after the switch; appends the time of every frame. The test holds no reference to the running scene, so the
	// scene the switch replaces goes within these frames. Returns the frames the load took.
	uint32_t MeasureSwitch(GameRuntime& game, AssetHandle target, std::vector<double>& frameTimes, const std::function<void()>& onSwitched)
	{
		const Timestep frame(1.0f / 60.0f);
		game.GetScene()->RequestSceneLoad(target);
		uint32_t loadingFrames = 0;
		uint32_t framesAfterSwitch = 0;
		auto next = std::chrono::steady_clock::now();
		const auto deadline = next + std::chrono::minutes(2);
		while (framesAfterSwitch < c_FramesAfterSwitch)
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			const uint64_t runningFrames = game.GetScene()->GetFrameIndex();
			const auto begin = std::chrono::steady_clock::now();
			game.Update(frame);
			frameTimes.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
			if (game.GetSceneHandle() == target)
			{
				if (framesAfterSwitch++ == 0)
					onSwitched();
			}
			else
			{
				// The running scene goes on every frame of the load.
				CHECK(game.GetScene()->GetFrameIndex() == runningFrames + 1);
				loadingFrames++;
			}
			next += c_FramePeriod;
			std::this_thread::sleep_until(next);
		}
		return loadingFrames;
	}

}

TEST_SUITE("Perf.Runtime")
{
	TEST_CASE("A game switches to a scene of 100,000 entities without stalling a frame")
	{
		Perf::ScopedApplicationJobSystem jobs;
		const std::filesystem::path manifest = WriteSwitchGame(CreateTemporaryDirectory("PerfRuntimeSwitch"));

		// A maximum by nature: the longest frame from the request to ten frames after the switch to the scene of 100,000
		// entities, and likewise switching away from it again (which replaces it), the hitches a switch must not cause.
		Perf::CheckBudgetConfirmed("Perf.Runtime.SceneSwitch100kMaxFrame", [&]
		{
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
			REQUIRE_MESSAGE(game, error);
			REQUIRE(game->WaitForStart());
			const Timestep frame(1.0f / 60.0f);
			for (int warmup = 0; warmup < 10; warmup++)
				game->Update(frame);

			std::vector<double> frameTimes;
			const uint32_t loadingFrames = MeasureSwitch(*game, UUID(c_BigScene), frameTimes, [&]()
			{
				// It starts only once everything it uses is in.
				for (uint32_t index = 0; index < c_Materials; index++)
					CHECK(game->GetAssetManager()->GetAssetState(UUID(c_FirstMaterial + index)) == AssetState::Ready);
				CHECK(game->GetScene()->GetEntityCount() == 100'002);
				CHECK(game->GetScene()->IsRunning());
			});
			CHECK(loadingFrames > 1);
			const double into = *std::max_element(frameTimes.begin(), frameTimes.end());
			const uint32_t leavingFrames = MeasureSwitch(*game, UUID(c_StartScene), frameTimes, [&]()
			{
				CHECK(game->GetScene()->GetEntityCount() == 1'002);
			});
			const double longest = *std::max_element(frameTimes.begin(), frameTimes.end());
			MESSAGE("Switching to 100,000 entities: ", loadingFrames, " frames of loading, the longest frame ", into, " ms; away from them: ",
				leavingFrames, " frames, the longest of all ", longest, " ms");
			return longest;
		});
	}
}
