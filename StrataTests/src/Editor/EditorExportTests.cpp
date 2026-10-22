#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Core/SettingsTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameExport.h"
#include "Editor/GameIcon.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetPack.h>
#include <Strata/Asset/RuntimeAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageReader.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project with a saved start scene that draws a material, an unused material, a prefab with a material of its own and
	// `fontMegabytes` of large font files (stored as they are; packing them takes frames, so exports show their progress).
	struct ExportHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path Directory;
		std::string ProjectDirectory;

		explicit ExportHarness(const std::string& name, uint32_t fontMegabytes = 0)
			: Directory(CreateTemporaryDirectory(name))
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory / "Game") }, { "name", "Export Game" } });
			Run("material.create", { { "path", "Materials/Used.stmat" } });
			Run("material.create", { { "path", "Materials/Unused.stmat" } });
			Run("material.create", { { "path", "Materials/ForEnemy.stmat" } });
			const std::string enemy = Run("entity.create", { { "name", "Enemy" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000001" },
				{ "Material", "Materials/ForEnemy.stmat" } } } } } })["id"].get<std::string>();
			Run("prefab.create", { { "entities", { enemy } }, { "path", "Prefabs/Enemy.stprefab" } });
			Run("entity.delete", { { "entities", { enemy } } });
			Run("entity.create", { { "name", "Box" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000001" }, { "Material", "Materials/Used.stmat" } } } } } });
			Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
			Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
			ProjectDirectory = Run("project.info")["directory"].get<std::string>();

			if (fontMegabytes > 0)
			{
				// Noise does not compress: the pack writer works through all of it.
				const std::filesystem::path fonts = Context.GetProject()->GetAssetDirectory() / "Fonts";
				REQUIRE(FileSystem::CreateDirectories(fonts));
				uint32_t state = 12345;
				for (uint32_t file = 0; file < 4; file++)
				{
					std::vector<uint8_t> bytes((static_cast<size_t>(fontMegabytes) << 20) / 4);
					for (uint8_t& byte : bytes)
					{
						state ^= state << 13;
						state ^= state >> 17;
						state ^= state << 5;
						byte = static_cast<uint8_t>(state);
					}
					REQUIRE(FileSystem::WriteBytes(fonts / ("Large" + std::to_string(file) + ".ttf"), bytes));
				}
				// Opened again, so that the new files are registered.
				Run("project.open", { { "path", ProjectDirectory } });
			}
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Start(const nlohmann::json& parameters)
		{
			return Commands.Execute(Context, "project.export", parameters);
		}

		std::string Error(std::string_view name, const nlohmann::json& parameters)
		{
			const EditorCommandResult result = CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			CHECK_FALSE(result.Success);
			return result.Error;
		}
	};

	// An RGBA8 image whose quadrants are red, green, blue and white (top left, top right, bottom left, bottom right), opaque.
	std::vector<uint8_t> MakeQuadrants(uint32_t width, uint32_t height)
	{
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, 255);
		for (uint32_t y = 0; y < height; y++)
		{
			for (uint32_t x = 0; x < width; x++)
			{
				uint8_t* pixel = &pixels[(static_cast<size_t>(y) * width + x) * 4];
				const bool right = x >= width / 2;
				const bool bottom = y >= height / 2;
				if (!right && !bottom)
					pixel[1] = pixel[2] = 0;
				else if (right && !bottom)
					pixel[0] = pixel[2] = 0;
				else if (!right && bottom)
					pixel[0] = pixel[1] = 0;
			}
		}
		return pixels;
	}

	// The pixel of an icon image at (x, y).
	std::array<uint8_t, 4> PixelAt(const GameIcon::Image& image, uint32_t x, uint32_t y)
	{
		const uint8_t* pixel = &image.Pixels[(static_cast<size_t>(y) * image.Size + x) * 4];
		return { pixel[0], pixel[1], pixel[2], pixel[3] };
	}

	// Whether a color is within a few steps of another (filters blur edges, and colors pass through linear light).
	bool IsNear(const std::array<uint8_t, 4>& color, std::array<uint8_t, 4> expected, int tolerance = 6)
	{
		for (size_t channel = 0; channel < 4; channel++)
		{
			if (std::abs(static_cast<int>(color[channel]) - static_cast<int>(expected[channel])) > tolerance)
				return false;
		}
		return true;
	}

	std::set<std::string> ExcludedPaths(const nlohmann::json& result)
	{
		std::set<std::string> paths;
		for (const nlohmann::json& asset : result["excludedAssets"])
			paths.insert(asset["path"].get<std::string>());
		return paths;
	}

}

TEST_SUITE("Editor.Export")
{
	TEST_CASE("Exports run over frames on an I/O thread and report their progress every frame")
	{
		ScopedJobSystem jobs(8, 2);
		ExportHarness harness("EditorExportProgress", 96);
		const std::filesystem::path output = harness.Directory / "Build";
		EditorCommandResult result = harness.Start({ { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } });
		REQUIRE(result.IsPending());
		const Ref<GameExport> exporter = harness.Context.GetGameExport();
		REQUIRE(exporter);

		// One export at a time.
		const EditorCommandResult second = harness.Commands.Execute(harness.Context, "project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Other") } });
		CHECK_FALSE(second.Success);
		CHECK(second.Error.find("running already") != std::string::npos);
		const nlohmann::json status = harness.Run("project.exportStatus");
		CHECK(status["running"] == true);

		// Polled frame by frame like the runner does: the progress never goes back, and frames keep coming while it packs.
		std::vector<float> progress;
		std::set<std::string> stages;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (result.IsPending() && std::chrono::steady_clock::now() < deadline)
		{
			harness.Context.Update(Timestep(1.0f / 60.0f));
			progress.push_back(exporter->GetProgress());
			stages.insert(GameExportStateToString(exporter->GetState()));
			// The I/O thread goes on meanwhile: the status reports the progress between the readings before and after it.
			const float before = exporter->GetProgress();
			const nlohmann::json frameStatus = harness.Run("project.exportStatus");
			const float after = exporter->GetProgress();
			CHECK(frameStatus["progress"].get<float>() >= before);
			CHECK(frameStatus["progress"].get<float>() <= after);
			PollCommandResult(result, harness.Context);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE_FALSE(result.IsPending());
		INFO(result.Error);
		REQUIRE(result.Success);
		for (size_t index = 1; index < progress.size(); index++)
			CHECK(progress[index] >= progress[index - 1]);
		CHECK(std::set<float>(progress.begin(), progress.end()).size() >= 3); // Steps between 0 and 1
		CHECK(exporter->GetProgress() == 1.0f);
		CHECK(stages.count("Packing") == 1);
		CHECK(exporter->GetState() == GameExportState::Succeeded);
		CHECK(exporter->GetStage().find("Exported") != std::string::npos);

		const nlohmann::json& value = result.Value;
		CHECK(value["frames"].get<uint64_t>() >= 3);
		CHECK(value["excludedAssets"].empty());
		CHECK(value["packBytes"].get<uint64_t>() >= 96ull << 20); // Noise is stored as it is
		CHECK(value["seconds"].get<double>() > 0.0);
		CHECK(FileSystem::GetFileSize(FileSystem::FromUTF8(value["assetPack"].get<std::string>())) == value["packBytes"].get<uint64_t>());
		const nlohmann::json finished = harness.Run("project.exportStatus");
		CHECK(finished["running"] == false);
		CHECK(finished["state"] == "Succeeded");
		CHECK(finished["progress"] == 1.0f);
		// The person hears about it (a toast), once.
		harness.Context.Update(Timestep(1.0f / 60.0f));
		harness.Context.Update(Timestep(1.0f / 60.0f));
		const auto countAnnouncements = [&harness]()
		{
			const std::deque<EditorNotification>& notifications = harness.Context.GetNotifications().GetActive();
			return std::count_if(notifications.begin(), notifications.end(), [](const EditorNotification& notification)
			{
				return notification.Level == NotificationLevel::Success && notification.Title == "Game exported" && notification.Count == 1;
			});
		};
		CHECK(countAnnouncements() == 1);
		// An export that does not start leaves the finished one, which is not announced again.
		const std::filesystem::path notADirectory = harness.Directory / "NotADirectory";
		REQUIRE(FileSystem::WriteText(notADirectory, "a file"));
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "project.export", { { "directory", FileSystem::ToUTF8(notADirectory) }, { "includeRuntime", false } }).Success);
		harness.Context.Update(Timestep(1.0f / 60.0f));
		harness.Context.Update(Timestep(1.0f / 60.0f));
		CHECK(countAnnouncements() == 1);

		// The game's pack starts with the start scene and what it uses.
		std::string error;
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(FileSystem::FromUTF8(value["assetPack"].get<std::string>()), &error);
		REQUIRE_MESSAGE(runtime, error);
		const std::vector<AssetPackEntry>& entries = runtime->GetPack().GetEntries();
		REQUIRE(entries.size() == value["assetCount"].get<size_t>());
		CHECK(entries[0].Metadata.Path == "Scenes/Main.stscene");
		CHECK(entries[1].Metadata.Path == "Materials/Used.stmat");
	}

	TEST_CASE("Cancelled exports and exports of closing projects stop")
	{
		ScopedJobSystem jobs(8, 2);
		ExportHarness harness("EditorExportCancel", 96);
		CHECK(harness.Error("project.cancelExport", {}).find("No export is running") != std::string::npos);

		const std::filesystem::path output = harness.Directory / "Build";
		EditorCommandResult result = harness.Start({ { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } });
		REQUIRE(result.IsPending());
		harness.Context.Update(Timestep(1.0f / 60.0f));
		harness.Run("project.cancelExport");
		result = CompleteCommand(harness.Context, std::move(result));
		CHECK_FALSE(result.Success);
		CHECK(result.ErrorKind == EditorCommandError::Cancelled);
		CHECK(harness.Context.GetGameExport()->GetState() == GameExportState::Cancelled);
		CHECK_FALSE(FileSystem::Exists(output / "Export Game.stpak")); // The writer removed what it wrote
		CHECK_FALSE(FileSystem::Exists(output / "Export Game.stgame"));

		// Closing the project cancels a running export and waits for it: the project's assets go only afterwards.
		result = harness.Start({ { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } });
		REQUIRE(result.IsPending());
		harness.Context.Update(Timestep(1.0f / 60.0f));
		harness.Run("project.close");
		CHECK(harness.Context.GetGameExport()->IsFinished());
		result = CompleteCommand(harness.Context, std::move(result));
		CHECK_FALSE(result.Success);
		CHECK(result.ErrorKind == EditorCommandError::Cancelled);
	}

	TEST_CASE("An export fails and names the scene when a scene saved meanwhile uses what the export did not plan for")
	{
		// One I/O thread, held busy: the export plans its pack at once, and writes it only once the thread is free, after the
		// edits below.
		ScopedJobSystem jobs(2, 1);
		ExportHarness harness("EditorExportEdited");
		harness.Run("scene.new");
		harness.Run("entity.create", { { "name", "Box" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000001" }, { "Material", "Materials/Used.stmat" } } } } } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Level2.stscene" } });
		harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" } });

		std::atomic<bool> release = false;
		std::atomic<bool> holding = false;
		const JobHandle blocker = JobSystem::SubmitIO([&]()
		{
			holding = true;
			while (!release)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		while (!holding)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		EditorCommandResult result = harness.Start({ { "directory", FileSystem::ToUTF8(harness.Directory / "Build") }, { "includeRuntime", false } });
		REQUIRE(result.IsPending());
		harness.Context.Update(Timestep(1.0f / 60.0f));
		REQUIRE(harness.Context.GetGameExport()->GetState() == GameExportState::Packing); // Planned; its writing waits

		// Meanwhile a new material, used by the other scene, which is saved. (Saved through the asset manager: opening the
		// scene would load it through the I/O thread held busy.)
		const std::string late = harness.Run("material.create", { { "path", "Materials/Late.stmat" } })["asset"].get<std::string>();
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		Scene level2("Level2");
		for (const AssetHandle material : { assets.FindAssetByPath("Materials/Used.stmat"), *UUIDFromJson(late) })
		{
			MeshRendererComponent& renderer = level2.CreateEntity("Drawn").AddComponent<MeshRendererComponent>();
			renderer.Mesh = UUID(1);
			renderer.Material = material;
		}
		const std::string document = JsonUtils::Dump(SceneSerializer::Serialize(level2));
		std::string error;
		REQUIRE_MESSAGE(assets.SaveNativeAsset(assets.FindAssetByPath("Scenes/Level2.stscene"),
			std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(document.data()), document.size()), false, &error), error);
		release = true;
		result = CompleteCommand(harness.Context, std::move(result));
		blocker.Wait();

		// The pack would hold the scene without the material: the export fails and says why, and leaves no game behind.
		CHECK_FALSE(result.Success);
		INFO(result.Error);
		CHECK(result.Error.find("Scenes/Level2.stscene") != std::string::npos);
		CHECK(result.Error.find("Materials/Late.stmat") != std::string::npos);
		CHECK(result.Error.find("export again") != std::string::npos);
		CHECK(harness.Context.GetGameExport()->GetState() == GameExportState::Failed);
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Build" / "Export Game.stpak"));
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Build" / "Export Game.stgame"));

		// Exported again, the game ships the scene with what it uses.
		const nlohmann::json value = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Build") }, { "includeRuntime", false } });
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(FileSystem::FromUTF8(value["assetPack"].get<std::string>()), &error);
		REQUIRE_MESSAGE(runtime, error);
		CHECK(runtime->FindAssetByPath("Materials/Late.stmat").IsValid());
	}

	TEST_CASE("An export plans once its imports are done: a document rewritten on disk meanwhile is scanned by the export, not waited for")
	{
		ScopedJobSystem jobs(2, 2);
		ExportHarness harness("EditorExportRewritten");
		std::vector<AssetHandle> materials;
		for (int index = 0; index < 10; index++)
		{
			const std::string material = harness.Run("material.create", { { "path", fmt::format("Materials/Rewrite{}.stmat", index) } })["asset"].get<std::string>();
			materials.push_back(*UUIDFromJson(material));
		}
		harness.Run("scene.new", { { "force", true } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Level2.stscene" } });
		harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" } });
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const AssetHandle level = assets.FindAssetByPath("Scenes/Level2.stscene");
		const std::filesystem::path file = assets.GetAbsolutePath(level);

		// Rewritten ten times by another program, each reported (as the file watcher reports it): one search waits for the file
		// to settle.
		const uint64_t searches = assets.GetReferenceSearchCount();
		for (const AssetHandle material : materials)
		{
			Scene rewritten("Level2");
			MeshRendererComponent& renderer = rewritten.CreateEntity("Drawn").AddComponent<MeshRendererComponent>();
			renderer.Mesh = UUID(1);
			renderer.Material = material;
			REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(SceneSerializer::Serialize(rewritten))));
			std::string error;
			REQUIRE_MESSAGE(assets.ReimportAssetInBackground(level, &error), error);
		}
		REQUIRE(assets.HasPendingReferenceSearches());
		CHECK_FALSE(assets.HasPendingImports());

		// The export plans without waiting for the search: it scans the document itself, and ships what it uses now.
		EditorCommandResult result = harness.Start({ { "directory", FileSystem::ToUTF8(harness.Directory / "Build") }, { "includeRuntime", false }, { "prune", true } });
		REQUIRE(result.IsPending());
		REQUIRE(WaitUntil([&]()
		{
			harness.Context.Update(Timestep(1.0f / 60.0f));
			PollCommandResult(result, harness.Context);
			return harness.Context.GetGameExport()->GetState() != GameExportState::WaitingForImports;
		}, std::chrono::seconds(30)));
		CHECK(assets.GetReferenceSearchCount() == searches); // Planned before the search started
		result = CompleteCommand(harness.Context, std::move(result));
		REQUIRE_MESSAGE(result.Success, result.Error);
		const std::set<std::string> excluded = ExcludedPaths(result.Value);
		CHECK_FALSE(excluded.contains("Materials/Rewrite9.stmat"));
		for (int index = 0; index < 9; index++)
			CHECK(excluded.contains(fmt::format("Materials/Rewrite{}.stmat", index)));

		// The ten reports made one search, of the latest contents.
		assets.WaitForImports();
		CHECK(assets.GetReferenceSearchCount() == searches + 1);
		CHECK(assets.GetDependencies(level) == std::vector<AssetHandle> { UUID(1), materials.back() });
	}

	TEST_CASE("Scenes and prefabs whose scripts reference themselves export")
	{
		// The start scene's rules restart it (a field naming the scene itself), and the enemy prefab spawns copies of itself.
		ExportHarness harness("EditorExportSelfReference");
		harness.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) } });
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const AssetHandle scene = assets.FindAssetByPath("Scenes/Main.stscene");
		const AssetHandle prefab = assets.FindAssetByPath("Prefabs/Enemy.stprefab");
		REQUIRE(scene.IsValid());
		REQUIRE(prefab.IsValid());
		const std::string rules = harness.Run("entity.create", { { "name", "Rules" } })["id"].get<std::string>();
		harness.Run("script.add", { { "entity", rules }, { "class", "GameFlow" }, { "fields", { { "Level", UUIDToJson(scene) } } } });
		const std::string spawner = harness.Run("entity.create", { { "name", "Spawner" } })["id"].get<std::string>();
		harness.Run("script.add", { { "entity", spawner }, { "class", "PendingSpawner" }, { "fields", { { "Prefab", UUIDToJson(prefab) } } } });
		harness.Run("scene.save");

		// The prefab is saved through the asset manager: no command edits a prefab.
		{
			Scene prefabScene("Enemy");
			Entity enemy = prefabScene.CreateEntity("Enemy");
			MeshRendererComponent& renderer = enemy.AddComponent<MeshRendererComponent>();
			renderer.Mesh = UUID(1);
			renderer.Material = assets.FindAssetByPath("Materials/ForEnemy.stmat");
			ScriptEntry script;
			script.ClassName = "PendingSpawner";
			script.Fields.push_back(ScriptFieldValue { "Prefab", PropertyType::Asset, prefab });
			enemy.AddComponent<ScriptComponent>().Scripts.push_back(std::move(script));
			const std::string document = JsonUtils::Dump(Prefab::CreateFromEntities(prefabScene, { enemy })->Serialize());
			std::string error;
			REQUIRE_MESSAGE(assets.SaveNativeAsset(prefab, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(document.data()), document.size()), true,
				&error), error);
		}

		// Both ship, and what they reference does not include themselves, in the editor or in the game.
		for (const bool prune : { false, true })
		{
			CAPTURE(prune);
			const std::filesystem::path output = harness.Directory / (prune ? "Pruned" : "Full");
			const nlohmann::json value = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false }, { "prune", prune } });
			if (prune)
				CHECK(ExcludedPaths(value) == std::set<std::string> { "Materials/Unused.stmat" });
			std::string error;
			const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(FileSystem::FromUTF8(value["assetPack"].get<std::string>()), &error);
			REQUIRE_MESSAGE(runtime, error);
			for (const AssetHandle handle : { scene, prefab })
			{
				const std::vector<AssetHandle> dependencies = runtime->GetDependencies(handle);
				CHECK(dependencies == assets.GetDependencies(handle));
				CHECK(std::find(dependencies.begin(), dependencies.end(), handle) == dependencies.end());
			}
			CHECK(runtime->FindAssetByPath("Materials/ForEnemy.stmat").IsValid());
		}
	}

	TEST_CASE("Pruned exports keep what the scenes use and what Export.AlwaysInclude names")
	{
		ExportHarness harness("EditorExportPrune");
		const std::filesystem::path output = harness.Directory / "Pruned";

		// Without pruning everything ships.
		const nlohmann::json full = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Full") }, { "includeRuntime", false } });
		CHECK(full["excludedAssets"].empty());

		// Pruned: the unused material, the prefab and its material stay home. The pack is planned once, when the export
		// begins to pack (planning follows every scene's dependencies: starting the export checks AlwaysInclude without it).
		const EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const uint64_t plans = assets.GetPackPlanCount();
		const nlohmann::json pruned = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false }, { "prune", true } });
		CHECK(assets.GetPackPlanCount() == plans + 1);
		CHECK(ExcludedPaths(pruned) == std::set<std::string> { "Materials/ForEnemy.stmat", "Materials/Unused.stmat", "Prefabs/Enemy.stprefab" });
		CHECK(pruned["assetCount"].get<size_t>() + 3 == full["assetCount"].get<size_t>());
		CHECK(pruned["packBytes"].get<uint64_t>() < full["packBytes"].get<uint64_t>());

		// A prefab scripts spawn by path is kept with what it uses once AlwaysInclude names it (saved in the project file).
		// Entries are checked without planning.
		CHECK(harness.Commands.Execute(harness.Context, "project.setExportSettings", { { "alwaysInclude", { "Prefabs/Missing.stprefab" } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(harness.Context, "project.setExportSettings", { { "alwaysInclude", "Prefabs" } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		const nlohmann::json settings = harness.Run("project.setExportSettings", { { "alwaysInclude", { "Prefabs" } } });
		CHECK(assets.GetPackPlanCount() == plans + 1);
		CHECK(settings["alwaysInclude"] == nlohmann::json { "Prefabs" });
		CHECK(harness.Run("project.info")["export"]["alwaysInclude"] == nlohmann::json { "Prefabs" });
		const std::optional<std::string> projectFile = FileSystem::ReadText(harness.Context.GetProject()->GetProjectFile());
		REQUIRE(projectFile);
		CHECK(projectFile->find("\"AlwaysInclude\"") != std::string::npos);

		const nlohmann::json kept = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false }, { "prune", true } });
		CHECK(assets.GetPackPlanCount() == plans + 2);
		CHECK(ExcludedPaths(kept) == std::set<std::string> { "Materials/Unused.stmat" });
		std::string error;
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(FileSystem::FromUTF8(kept["assetPack"].get<std::string>()), &error);
		REQUIRE_MESSAGE(runtime, error);
		CHECK(runtime->FindAssetByPath("Prefabs/Enemy.stprefab").IsValid());
		CHECK(runtime->FindAssetByPath("Materials/ForEnemy.stmat").IsValid());
		CHECK_FALSE(runtime->FindAssetByPath("Materials/Unused.stmat").IsValid());
		CHECK(GameManifest::Load(FileSystem::FromUTF8(kept["manifest"].get<std::string>())).has_value());

		// A list that names what is gone (the project file was edited, or the prefab deleted) fails a pruned export at once,
		// naming the entry, before anything is planned or written.
		harness.Context.GetProject()->GetConfig().Export.AlwaysInclude = { "Prefabs", "Prefabs/Boss.stprefab" };
		const std::filesystem::path stale = harness.Directory / "Stale";
		CHECK(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(stale) }, { "includeRuntime", false }, { "prune", true } })
			== "Export.AlwaysInclude names 'Prefabs/Boss.stprefab', which is no asset or folder of the project");
		CHECK_FALSE(FileSystem::Exists(stale));
		CHECK(assets.GetPackPlanCount() == plans + 2);

		// Clearing the list again.
		harness.Run("project.setExportSettings", { { "alwaysInclude", nlohmann::json::array() } });
		CHECK(harness.Run("project.info")["export"]["alwaysInclude"].empty());
	}

	TEST_CASE("An export carries the project's settings sections into its manifest")
	{
		ExportHarness harness("EditorExportSettings");
		// The game's manifest has the project's sections, also those no module of this build claims (as they are stored).
		const nlohmann::json settings = { { "Game.Difficulty", { { "Version", 2 }, { "Value", { { "Level", "Hard" } } } } } };
		std::string error;
		std::optional<ProjectSettingsValues> values = ProjectSettingsValues::FromJson(settings, &error);
		REQUIRE_MESSAGE(values, error);
		harness.Context.GetProject()->GetSettings() = std::move(*values);
		const nlohmann::json result = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Settings") }, { "includeRuntime", false } });
		std::optional<GameManifest> manifest = GameManifest::Load(FileSystem::FromUTF8(result["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(manifest, error);
		REQUIRE(manifest->Settings);
		CHECK(manifest->Settings->ToJson() == settings);

		// Without sections the manifest carries an empty set: the game provides the modules' defaults.
		harness.Context.GetProject()->GetSettings() = ProjectSettingsValues();
		const nlohmann::json plain = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Plain") }, { "includeRuntime", false } });
		manifest = GameManifest::Load(FileSystem::FromUTF8(plain["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(manifest, error);
		REQUIRE(manifest->Settings);
		CHECK(manifest->Settings->IsEmpty());
	}

	TEST_CASE("A pruned export keeps an asset a section names")
	{
		if (!IsSettingsSectionProcess())
			return RunInSettingsSectionProcess();
		ExportHarness harness("EditorExportSectionRoots");
		// A texture nothing but the game's settings names.
		const std::filesystem::path source = CreateTemporaryDirectory("EditorExportSectionRootsSource") / "Banner.png";
		REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(4, 4, 200, 40, 40)));
		const nlohmann::json imported = harness.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } });
		const std::string banner = imported["asset"].get<std::string>();
		const std::string bannerPath = imported["path"].get<std::string>();

		const nlohmann::json without = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Without") },
			{ "includeRuntime", false }, { "prune", true } });
		CHECK(ExcludedPaths(without).contains(bannerPath));

		harness.Run("project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Banner", banner } } } });
		const nlohmann::json with = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "With") },
			{ "includeRuntime", false }, { "prune", true } });
		CHECK_FALSE(ExcludedPaths(with).contains(bannerPath));
		std::string error;
		const std::optional<GameManifest> manifest = GameManifest::Load(FileSystem::FromUTF8(with["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(manifest, error);
		REQUIRE(manifest->Settings);
		REQUIRE(manifest->Settings->IsStored(c_GameTestSection));
		CHECK(manifest->Settings->GetStoredEntry(c_GameTestSection)["Value"]["Banner"] == banner);
	}

	TEST_CASE("The manifest carries title, version, company, copyright and the icon file; the PNG decodes at 256 pixels")
	{
		ExportHarness harness("EditorExportIdentity");
		// Without an identity the game shows the project's name and has no icon.
		const nlohmann::json plain = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Plain") }, { "includeRuntime", false } });
		CHECK(plain["icon"].is_null());
		CHECK(plain["warnings"].empty());
		std::string error;
		std::optional<GameManifest> manifest = GameManifest::Load(FileSystem::FromUTF8(plain["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(manifest, error);
		CHECK(manifest->Title == "Export Game");
		CHECK(manifest->Version == ProjectGameInfo::c_DefaultVersion);
		CHECK(manifest->Icon.empty());

		const std::filesystem::path source = CreateTemporaryDirectory("EditorExportIdentitySource") / "Icon.png";
		REQUIRE(FileSystem::WriteBytes(source, EncodePNG(300, 300, MakeQuadrants(300, 300))));
		const std::string icon = harness.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		harness.Run("project.setGameInfo", { { "title", "Export Adventure" }, { "version", "2.3.4" }, { "company", "Studio" },
			{ "copyright", "(c) 2026 Studio" }, { "icon", icon } });
		const std::filesystem::path output = harness.Directory / "Identity";
		const nlohmann::json result = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false }, { "prune", true } });
		CHECK(result["warnings"].empty());
		REQUIRE(result["icon"].is_string());
		CHECK(FileSystem::FromUTF8(result["icon"].get<std::string>()) == output / "Export Game.png");
		manifest = GameManifest::Load(output / "Export Game.stgame", &error);
		REQUIRE_MESSAGE(manifest, error);
		// The name keys the saves; the title is what the game shows.
		CHECK(manifest->Name == "Export Game");
		CHECK(manifest->Title == "Export Adventure");
		CHECK(manifest->Version == "2.3.4");
		CHECK(manifest->Company == "Studio");
		CHECK(manifest->Copyright == "(c) 2026 Studio");
		CHECK(manifest->Icon == "Export Game.png");
		// The icon is a file of its own: a pruned pack need not hold its texture.
		CHECK(ExcludedPaths(result).contains("Textures/Icon.png"));

		const std::optional<std::vector<uint8_t>> png = FileSystem::ReadBytes(output / "Export Game.png");
		REQUIRE(png);
		const std::optional<ReadbackImage> decoded = ImageReader::DecodeRGBA8(*png, &error);
		REQUIRE_MESSAGE(decoded, error);
		CHECK(decoded->Width == 256);
		CHECK(decoded->Height == 256);
		const uint8_t* topLeft = &decoded->Pixels[(32 * 256 + 32) * 4];
		CHECK(topLeft[0] > 240);
		CHECK(topLeft[1] < 16);
		CHECK(topLeft[3] == 255);
	}

	TEST_CASE("Deleting the icon texture is refused while the project names it")
	{
		ExportHarness harness("EditorExportIconReference");
		const std::filesystem::path source = CreateTemporaryDirectory("EditorExportIconReferenceSource") / "Icon.png";
		REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(4, 4, 40, 40, 200)));
		const std::string icon = harness.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		harness.Run("project.setGameInfo", { { "icon", icon } });

		const std::string refused = harness.Error("asset.delete", { { "asset", icon } });
		CHECK(refused.find("Export Game.stproj") != std::string::npos);
		CHECK(refused.find("the game's icon") != std::string::npos);
		const nlohmann::json references = harness.Run("asset.findReferences", { { "asset", icon } })["referencedBy"];
		REQUIRE(references.size() == 1);
		CHECK(references[0]["type"] == "Project");
		CHECK(references[0]["settings"] == nlohmann::json { "gameIcon" });

		// Once the project no longer names it, it can go.
		harness.Run("project.setGameInfo", { { "icon", nullptr } });
		harness.Run("asset.delete", { { "asset", icon } });
		CHECK_FALSE(harness.Context.GetAssetManager()->GetMetadata(UUIDFromJson(nlohmann::json(icon)).value_or(UUID::Null())));
	}

	TEST_CASE("An export refuses a project whose section fails Read, naming it")
	{
		if (!IsSettingsSectionProcess())
			return RunInSettingsSectionProcess();
		ExportHarness harness("EditorExportInvalidSection");
		std::string error;
		std::optional<ProjectSettingsValues> values = ProjectSettingsValues::FromJson({ { c_GameTestSection, { { "Version", 1 },
			{ "Value", { { "Lives", 99 } } } } } }, &error);
		REQUIRE_MESSAGE(values, error);
		harness.Context.GetProject()->GetSettings() = std::move(*values);
		const std::string refused = harness.Error("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Refused") },
			{ "includeRuntime", false } });
		CHECK(refused.find(c_GameTestSection) != std::string::npos);
		CHECK(refused.find("Lives") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Refused" / "Export Game.stgame"));
	}

	TEST_CASE("An export's frame times start with the frame that started it")
	{
		ScopedJobSystem jobs(8, 2);
		ExportHarness harness("EditorExportFrameTimes", 8);
		// Frames as the editor layer runs them: the time of the frame before, then the commands. The frame that starts the
		// export does its checks and reads the script module (40 ms here); the export answers for it.
		harness.Context.RecordFrameTime(1.0);
		EditorCommandResult result = harness.Start({ { "directory", FileSystem::ToUTF8(harness.Directory / "Build") }, { "includeRuntime", false } });
		REQUIRE(result.IsPending());
		double frameTime = 40.0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (result.IsPending() && std::chrono::steady_clock::now() < deadline)
		{
			harness.Context.RecordFrameTime(frameTime);
			frameTime = 2.0;
			harness.Context.Update(Timestep(1.0f / 60.0f));
			PollCommandResult(result, harness.Context);
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE_FALSE(result.IsPending());
		INFO(result.Error);
		REQUIRE(result.Success);
		const nlohmann::json& frameTimes = result.Value["frameTimes"];
		CHECK(frameTimes["count"] == result.Value["frames"]);
		CHECK(frameTimes["maxMs"] == 40.0);
	}

	TEST_CASE("Export directories are checked before anything is written, with what to do instead")
	{
		ExportHarness harness("EditorExportDirectories");
		const std::filesystem::path project = harness.Context.GetProject()->GetProjectDirectory();
		auto error = [&](const std::string& directory) { return harness.Error("project.export", { { "directory", directory }, { "includeRuntime", false } }); };

		CHECK(error("").find("Choose the directory") != std::string::npos);
		CHECK(error("relative/Build").find("absolute path") != std::string::npos);
		CHECK(error("//server/share/Build").find("network or device path") != std::string::npos);
		CHECK(error(FileSystem::ToUTF8(project / "Build")).find("inside the project directory") != std::string::npos);
		CHECK(error(FileSystem::ToUTF8(harness.Directory / "Games" / "con" / "Build")).find("reserved device name") != std::string::npos);
		const std::filesystem::path file = harness.Directory / "AFile";
		REQUIRE(FileSystem::WriteText(file, "not a directory"));
		CHECK(error(FileSystem::ToUTF8(file)).find("is a file, not a directory") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Games"));
		CHECK_FALSE(CheckExportDirectory(harness.Directory / "Build", project).has_value());
	}
}

TEST_SUITE("Editor.Export.GameIcon")
{
	TEST_CASE("A square icon is resized to every size")
	{
		std::string error;
		const std::optional<GameIcon::Images> icon = GameIcon::Convert(EncodePNG(512, 512, MakeQuadrants(512, 512)), &error);
		REQUIRE_MESSAGE(icon, error);
		CHECK(icon->SourceWidth == 512);
		CHECK(icon->SourceHeight == 512);
		CHECK(icon->Warnings.empty());
		REQUIRE(icon->Sizes.size() == GameIcon::c_Sizes.size());
		for (size_t index = 0; index < GameIcon::c_Sizes.size(); index++)
		{
			const GameIcon::Image& image = icon->Sizes[index];
			const uint32_t size = GameIcon::c_Sizes[index];
			CAPTURE(size);
			REQUIRE(image.Size == size);
			REQUIRE(image.Pixels.size() == static_cast<size_t>(size) * size * 4);
			// Each quadrant keeps its color away from the edges between them.
			const uint32_t first = size / 8;
			const uint32_t last = size - 1 - size / 8;
			CHECK(IsNear(PixelAt(image, first, first), { 255, 0, 0, 255 }));
			CHECK(IsNear(PixelAt(image, last, first), { 0, 255, 0, 255 }));
			CHECK(IsNear(PixelAt(image, first, last), { 0, 0, 255, 255 }));
			CHECK(IsNear(PixelAt(image, last, last), { 255, 255, 255, 255 }));
		}
		CHECK(icon->GetLargest().Size == GameIcon::c_LargestSize);

		// The largest image's PNG decodes as it is.
		const std::optional<std::vector<uint8_t>> png = GameIcon::EncodePNG(icon->GetLargest(), &error);
		REQUIRE_MESSAGE(png, error);
		const std::optional<ReadbackImage> decoded = ImageReader::DecodeRGBA8(*png, &error);
		REQUIRE_MESSAGE(decoded, error);
		CHECK(decoded->Width == 256);
		CHECK(decoded->Pixels == icon->GetLargest().Pixels);
	}

	TEST_CASE("A non-square icon is centered on transparent pixels")
	{
		// 400 x 200 of red: the 400-pixel square has 100 transparent rows above and below it.
		std::vector<uint8_t> red(400 * 200 * 4, 0);
		for (size_t pixel = 0; pixel < 400 * 200; pixel++)
		{
			red[pixel * 4] = 255;
			red[pixel * 4 + 3] = 255;
		}
		std::string error;
		const std::optional<GameIcon::Images> icon = GameIcon::Convert(EncodePNG(400, 200, red), &error);
		REQUIRE_MESSAGE(icon, error);
		CHECK(icon->Warnings.empty());
		const GameIcon::Image& large = icon->GetLargest();
		CHECK(PixelAt(large, 128, 10)[3] == 0);
		CHECK(PixelAt(large, 128, 245)[3] == 0);
		CHECK(IsNear(PixelAt(large, 128, 128), { 255, 0, 0, 255 }));
		CHECK(IsNear(PixelAt(large, 5, 128), { 255, 0, 0, 255 }));
		// The transparent border neither darkens nor tints the edge: colors are weighted by alpha.
		for (uint32_t row = 56; row < 72; row++)
		{
			const std::array<uint8_t, 4> pixel = PixelAt(large, 128, row);
			if (pixel[3] > 8)
			{
				CAPTURE(row);
				CHECK(pixel[0] > 240);
				CHECK(pixel[1] < 8);
			}
		}
		const GameIcon::Image& small = icon->Sizes.front();
		CHECK(small.Size == 16);
		CHECK(IsNear(PixelAt(small, 8, 8), { 255, 0, 0, 255 }));
		CHECK(PixelAt(small, 8, 0)[3] == 0);
	}

	TEST_CASE("A small icon is upscaled with a warning")
	{
		std::string error;
		const std::optional<GameIcon::Images> icon = GameIcon::Convert(CreateSolidPNG(64, 48, 40, 200, 40), &error);
		REQUIRE_MESSAGE(icon, error);
		REQUIRE(icon->Warnings.size() == 1);
		CHECK(icon->Warnings[0].find("64 x 48 pixels") != std::string::npos);
		CHECK(icon->Warnings[0].find("at least 256 x 256") != std::string::npos);
		REQUIRE(icon->Sizes.size() == GameIcon::c_Sizes.size());
		CHECK(IsNear(PixelAt(icon->GetLargest(), 128, 128), { 40, 200, 40, 255 }));
		// The sizes of the source and smaller are not enlarged, so no warning for a source of 256 pixels.
		const std::optional<GameIcon::Images> exact = GameIcon::Convert(CreateSolidPNG(256, 256, 40, 200, 40), &error);
		REQUIRE_MESSAGE(exact, error);
		CHECK(exact->Warnings.empty());

		// The export reports the warning.
		ExportHarness harness("EditorExportSmallIcon");
		const std::filesystem::path source = CreateTemporaryDirectory("EditorExportSmallIconSource") / "Small.png";
		REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(64, 64, 40, 200, 40)));
		const std::string handle = harness.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		harness.Run("project.setGameInfo", { { "icon", handle } });
		const nlohmann::json result = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(harness.Directory / "Small") }, { "includeRuntime", false } });
		REQUIRE(result["warnings"].size() == 1);
		CHECK(result["warnings"][0].get<std::string>().find("64 x 64 pixels") != std::string::npos);
	}

	TEST_CASE("An icon that does not decode fails the export")
	{
		std::string error;
		const std::vector<uint8_t> garbage = { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e' };
		CHECK_FALSE(GameIcon::Convert(garbage, &error));
		CHECK(error.find("no image the icon can be made from") != std::string::npos);

		ExportHarness harness("EditorExportBrokenIcon");
		const std::filesystem::path source = CreateTemporaryDirectory("EditorExportBrokenIconSource") / "Broken.png";
		REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(256, 256, 40, 200, 40)));
		const std::string handle = harness.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		harness.Run("project.setGameInfo", { { "icon", handle } });
		// The file changes on disk after it was imported (nothing watches it here): the export decodes the file as it is.
		const std::filesystem::path imported = harness.Context.GetAssetManager()->GetAbsolutePath(UUIDFromJson(nlohmann::json(handle)).value_or(UUID::Null()));
		REQUIRE(FileSystem::WriteBytes(imported, garbage));
		const std::filesystem::path output = harness.Directory / "Broken";
		const std::string refused = harness.Error("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } });
		CHECK(refused.find("The game's icon") != std::string::npos);
		CHECK(refused.find("Broken.png") != std::string::npos);
		CHECK(refused.find("project.setGameInfo") != std::string::npos);
		// It failed before writing the pack or the manifest.
		CHECK_FALSE(FileSystem::Exists(output / "Export Game.stgame"));
		CHECK_FALSE(FileSystem::Exists(output / "Export Game.stpak"));
	}
}
