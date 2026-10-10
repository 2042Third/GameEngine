#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>

using namespace Strata;

namespace
{

	constexpr uint64_t c_MiB = 1024 * 1024;

	struct StreamingHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters)
		{
			return Commands.Execute(Context, name, parameters);
		}

		void Frame()
		{
			Context.Update(Timestep(1.0f / 60.0f));
		}
	};

	nlohmann::json Budget(uint64_t bytes)
	{
		return bytes == AssetResidencyBudgets::c_Unlimited ? nlohmann::json(nullptr) : nlohmann::json(bytes);
	}

}

TEST_SUITE("Editor.Streaming")
{
	TEST_CASE("asset.stats reports the asset manager's statistics, and lists its assets on request")
	{
		StreamingHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorStreamingStats");
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		const std::string red = harness.Run("material.create", { { "path", "Materials/Red.stmat" },
			{ "properties", { { "BaseColor", { 1, 0, 0, 1 } } } } })["asset"].get<std::string>();
		const std::string blue = harness.Run("material.create", { { "path", "Materials/Blue.stmat" },
			{ "properties", { { "BaseColor", { 0, 0, 1, 1 } } } } })["asset"].get<std::string>();
		const AssetHandle redHandle = *UUIDFromJson(red);
		const AssetHandle blueHandle = *UUIDFromJson(blue);
		REQUIRE(AssetManager::LoadAssetSync<Material>(redHandle));
		REQUIRE(AssetManager::LoadAssetSync<Material>(blueHandle));
		harness.Frame();
		AssetManager::GetAsset<Material>(blueHandle); // Used most recently
		harness.Frame();

		const AssetManagerStats stats = AssetManager::GetActive()->GetStats();
		const nlohmann::json totals = harness.Run("asset.stats");
		CHECK_FALSE(totals.contains("assets"));
		CHECK(totals["frame"] == stats.Frame);
		CHECK(totals["registered"] == stats.RegisteredAssets);
		CHECK(totals["loaded"] == stats.LoadedAssets);
		CHECK(totals["loading"] == stats.LoadingAssets);
		CHECK(totals["failed"] == stats.FailedAssets);
		CHECK(totals["pinned"] == stats.PinnedAssets);
		CHECK(totals["loadedBytes"] == stats.LoadedMemory);
		CHECK(totals["loadsCompleted"] == stats.TotalLoadsCompleted);
		CHECK(totals["pools"]["cpu"]["residentBytes"] == stats.Resident.Cpu);
		CHECK(totals["pools"]["gpuTextures"]["residentBytes"] == stats.Resident.GpuTextures);
		CHECK(totals["pools"]["gpuBuffers"]["residentBytes"] == stats.Resident.GpuBuffers);
		CHECK(totals["pools"]["cpu"]["budgetBytes"] == Budget(stats.Budgets.Cpu));
		CHECK(totals["pools"]["gpuTextures"]["budgetBytes"] == Budget(stats.Budgets.GpuTextures));
		CHECK(totals["streaming"]["queued"] == nlohmann::json { { "high", 0 }, { "normal", 0 }, { "low", 0 } });
		CHECK(totals["streaming"]["inFlightBytes"] == stats.InFlightBytes);
		CHECK(totals["streaming"]["inFlightBudgetBytes"] == stats.Budgets.InFlightBytes);
		CHECK(totals["finalization"]["uploadBudgetBytesPerFrame"] == stats.Budgets.UploadBytesPerFrame);
		CHECK(totals["finalization"]["windowFrames"] == AssetManagerBase::c_StatsWindowFrames);
		CHECK(totals["evictions"] == stats.Evictions);
		CHECK(totals["cancellations"] == stats.Cancellations);
		CHECK(totals["stagingReleases"] == stats.StagingReleases);

		// The list: by memory (the default) or by last use, limited.
		const nlohmann::json listed = harness.Run("asset.stats", { { "assets", true } });
		CHECK(listed["assetCount"] == listed["assets"].size());
		bool foundRed = false;
		for (const nlohmann::json& asset : listed["assets"])
		{
			if (asset["handle"] != red)
				continue;
			foundRed = true;
			CHECK(asset["path"] == "Materials/Red.stmat");
			CHECK(asset["type"] == "Material");
			CHECK(asset["state"] == "Ready");
			CHECK(asset["cpuBytes"] == sizeof(Material));
			CHECK(asset["gpuBytes"] == 0);
			CHECK(asset["pins"] == 0);
			CHECK(asset["memoryAsset"] == false);
		}
		CHECK(foundRed);
		for (size_t index = 1; index < listed["assets"].size(); index++)
		{
			const nlohmann::json& previous = listed["assets"][index - 1];
			const nlohmann::json& current = listed["assets"][index];
			const auto bytes = [](const nlohmann::json& asset) { return asset["cpuBytes"].get<uint64_t>() + asset["gpuBytes"].get<uint64_t>(); };
			CHECK(bytes(previous) >= bytes(current));
		}
		const nlohmann::json recent = harness.Run("asset.stats", { { "assets", true }, { "sort", "lastUsed" }, { "limit", 1 } });
		REQUIRE(recent["assets"].size() == 1);
		CHECK(recent["assets"][0]["handle"] == blue);
		CHECK(recent["assetCount"] == listed["assetCount"]);

		CHECK(harness.Execute("asset.stats", { { "sort", "size" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Execute("asset.stats", { { "limit", 0 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Execute("asset.stats", { { "assets", "yes" } }).ErrorKind == EditorCommandError::InvalidParameters);

		// editor.status carries the same totals.
		const nlohmann::json status = harness.Run("editor.status");
		REQUIRE(status.contains("assets"));
		CHECK(status["assets"]["registered"] == stats.RegisteredAssets);
		CHECK(status["assets"]["pools"]["cpu"]["residentBytes"] == AssetManager::GetActive()->GetStats().Resident.Cpu);
	}

	TEST_CASE("asset.setBudget changes the budgets, rejects values that are not positive, and resets them")
	{
		StreamingHarness harness;
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive(); // Without a project: the built-in assets' manager
		REQUIRE(manager);
		const AssetResidencyBudgets defaults = AssetManagerBase::GetDefaultResidencyBudgets();

		const nlohmann::json set = harness.Run("asset.setBudget", { { "gpuTexturesMB", 64 }, { "gpuBuffersMB", 16 }, { "cpuMB", 0.5 }, { "inFlightMB", 32 },
			{ "uploadMBPerFrame", 8 }, { "finalizeMsPerFrame", 2.5 } });
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		CHECK(budgets.GpuTextures == 64 * c_MiB);
		CHECK(budgets.GpuBuffers == 16 * c_MiB);
		CHECK(budgets.Cpu == c_MiB / 2);
		CHECK(budgets.InFlightBytes == 32 * c_MiB);
		CHECK(budgets.UploadBytesPerFrame == 8 * c_MiB);
		CHECK(budgets.FinalizeMsPerFrame == doctest::Approx(2.5f));
		CHECK(set["budgets"]["gpuTexturesBytes"] == 64 * c_MiB);
		CHECK(set["budgets"]["cpuBytes"] == c_MiB / 2);
		CHECK(set["budgets"]["finalizeMsPerFrame"].get<double>() == doctest::Approx(2.5));
		CHECK(harness.Run("asset.stats")["pools"]["gpuTextures"]["budgetBytes"] == 64 * c_MiB);

		// Values that are not positive numbers change nothing.
		for (const nlohmann::json& parameters : { nlohmann::json { { "gpuTexturesMB", 0 } }, nlohmann::json { { "cpuMB", -1 } },
				 nlohmann::json { { "finalizeMsPerFrame", 0.0 } }, nlohmann::json { { "inFlightMB", "lots" } }, nlohmann::json { { "uploadMBPerFrame", 1e300 } },
				 nlohmann::json { { "gpuTexturesMB", 128 }, { "gpuBuffersMB", 0 } } })
		{
			CAPTURE(parameters.dump());
			CHECK(harness.Execute("asset.setBudget", parameters).ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(manager->GetResidencyBudgets().GpuTextures == 64 * c_MiB);
		}

		// Only what is given changes; reset starts from the defaults.
		harness.Run("asset.setBudget", { { "cpuMB", 2 } });
		CHECK(manager->GetResidencyBudgets().GpuTextures == 64 * c_MiB);
		CHECK(manager->GetResidencyBudgets().Cpu == 2 * c_MiB);
		const nlohmann::json reset = harness.Run("asset.setBudget", { { "reset", true }, { "uploadMBPerFrame", 4 } });
		budgets = manager->GetResidencyBudgets();
		CHECK(budgets.GpuTextures == defaults.GpuTextures);
		CHECK(budgets.Cpu == defaults.Cpu);
		CHECK(budgets.UploadBytesPerFrame == 4 * c_MiB);
		CHECK(reset["budgets"]["cpuBytes"] == Budget(defaults.Cpu));
	}

	TEST_CASE("Opening another scene releases the assets only the previous scene used")
	{
		StreamingHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorStreamingSwitch");
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		const AssetHandle first = *UUIDFromJson(harness.Run("material.create", { { "path", "Materials/First.stmat" } })["asset"]);
		const AssetHandle shared = *UUIDFromJson(harness.Run("material.create", { { "path", "Materials/Shared.stmat" } })["asset"]);
		const AssetHandle second = *UUIDFromJson(harness.Run("material.create", { { "path", "Materials/Second.stmat" } })["asset"]);
		harness.Run("scene.saveAs", { { "path", "Scenes/First.stscene" } });
		harness.Run("scene.new");
		harness.Run("scene.saveAs", { { "path", "Scenes/Second.stscene" } });
		harness.Run("scene.open", { { "scene", "Scenes/First.stscene" } });

		// What draws the scene requests its assets every frame (here: the test, as no viewport renders).
		const auto frame = [&harness](std::initializer_list<AssetHandle> used)
		{
			for (AssetHandle handle : used)
				AssetManager::GetActive()->GetAsset(handle);
			harness.Frame();
		};
		for (int warmup = 0; warmup < 3; warmup++)
			frame({ first, shared });
		REQUIRE(AssetManager::GetAssetState(first) == AssetState::Ready);
		REQUIRE(AssetManager::GetAssetState(shared) == AssetState::Ready);

		harness.Run("scene.open", { { "scene", "Scenes/Second.stscene" } });
		for (uint32_t switched = 0; switched < AssetResidency::c_SceneSwitchTrimFrames + 1; switched++)
			frame({ shared, second });
		CHECK(AssetManager::GetAssetState(first) == AssetState::Unloaded);
		CHECK(AssetManager::GetAssetState(shared) == AssetState::Ready);
		CHECK(AssetManager::GetAssetState(second) == AssetState::Ready);
		CHECK(harness.Run("asset.stats")["evictions"].get<uint64_t>() >= 1);
	}
}
