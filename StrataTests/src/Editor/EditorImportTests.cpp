#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Font.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// Imports ".stgated" files as fonts (the embedded default font), but only once the test opens its gate, so a test can
	// hold an import while the editor's frames go on. A held import that is cancelled stops, as a long import does at its
	// next cancellation check. Settings: {"Level": 0..10, "Fail": bool}.
	class GatedImporter final : public AssetImporter
	{
	public:
		static inline std::mutex s_Mutex;
		static inline std::condition_variable s_Condition;
		static inline bool s_GateOpen = true;
		static inline uint32_t s_Waiting = 0;
		static inline std::atomic<uint32_t> s_ImportCount = 0;
		static inline std::atomic<uint32_t> s_CancelledCount = 0; // Held imports that stopped because they were cancelled
		static inline std::atomic<int64_t> s_LastLevel = -1;

		static void SetGate(bool open)
		{
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_GateOpen = open;
			}
			s_Condition.notify_all();
		}

		// Whether an import waits at the closed gate.
		static bool IsWaiting()
		{
			return GetWaiting() > 0;
		}

		// How many imports wait at the closed gate.
		static uint32_t GetWaiting()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_Waiting;
		}

		AssetType GetType() const override { return AssetType::Font; }
		std::vector<std::string> GetExtensions() const override { return { ".stgated" }; }
		uint32_t GetVersion() const override { return 1; }

		nlohmann::json GetDefaultSettings(const std::filesystem::path&) const override
		{
			return { { "Level", 0 }, { "Fail", false } };
		}

		std::vector<PropertyInfo> GetSettingsProperties() const override
		{
			std::vector<PropertyInfo> properties;
			ImportSettingsBuilder(properties)
				.UInt("Level", 0, 10).Tooltip("A number the import reads")
				.Bool("Fail", false).Tooltip("Fail the import");
			return properties;
		}

		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
		{
			{
				std::unique_lock<std::mutex> lock(s_Mutex);
				s_Waiting++;
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				while (!s_GateOpen && !context.IsCancelled() && std::chrono::steady_clock::now() < deadline)
					s_Condition.wait_for(lock, std::chrono::milliseconds(1));
				s_Waiting--;
				if (context.IsCancelled())
				{
					s_CancelledCount++;
					*outError = "cancelled";
					return false;
				}
				if (!s_GateOpen)
				{
					*outError = "The test never opened the gate";
					return false;
				}
			}
			s_ImportCount++;
			s_LastLevel = JsonUtils::GetInt(context.Settings, "Level", -1);
			if (JsonUtils::GetBool(context.Settings, "Fail", false))
			{
				*outError = "Requested failure";
				return false;
			}
			result.Data = Font::GetDefault()->GetData();
			return true;
		}
	};

	void RegisterGatedImporter()
	{
		static bool s_Registered = false;
		if (!s_Registered)
		{
			AssetImporterRegistry::Register(CreateScope<GatedImporter>());
			s_Registered = true;
		}
	}

	// Opens the gate when the test ends, also when it fails, before the job system shuts down (declare it after the jobs).
	struct OpenGateAtEnd
	{
		~OpenGateAtEnd()
		{
			GatedImporter::SetGate(true);
		}
	};

	double SecondsOf(const std::function<void()>& function)
	{
		const auto start = std::chrono::steady_clock::now();
		function();
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}

	// What an editor action that never waits for an import may take, generous for loaded machines: waiting for a held
	// import takes the gate's minute.
	constexpr double c_AtOnceSeconds = 2.0;

	// An editor with a project, whose frames (context update, then the runner) a test runs one at a time.
	struct ImportHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::filesystem::path Assets;

		explicit ImportHarness(const std::string& name)
		{
			const std::filesystem::path directory = Tests::CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
			Assets = Context.GetProject()->GetAssetDirectory();
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		// Writes a file into the asset directory and imports it (gate open), returning its handle.
		std::string Add(const std::string& relativePath, const std::vector<uint8_t>& bytes)
		{
			REQUIRE(FileSystem::CreateDirectories((Assets / FileSystem::FromUTF8(relativePath)).parent_path()));
			const std::filesystem::path external = Assets.parent_path() / "External" / FileSystem::FromUTF8(relativePath).filename();
			REQUIRE(FileSystem::CreateDirectories(external.parent_path()));
			REQUIRE(FileSystem::WriteBytes(external, bytes));
			const std::string directory = FileSystem::ToUTF8(FileSystem::FromUTF8(relativePath).parent_path());
			return Run("asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", directory } })["asset"].get<std::string>();
		}

		// Starts a command through the runner; the returned slot holds its result once it completed.
		std::shared_ptr<std::optional<EditorCommandResult>> Start(std::string_view name, const nlohmann::json& parameters)
		{
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			Runner.Run(Context, Commands, name, parameters, [completed](const EditorCommandResult& result) { *completed = result; });
			return completed;
		}

		void Frame()
		{
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
		}

		EditorCommandResult Finish(const std::shared_ptr<std::optional<EditorCommandResult>>& completed)
		{
			REQUIRE(Tests::WaitUntil([&]()
			{
				Frame();
				return completed->has_value();
			}, std::chrono::seconds(60)));
			return **completed;
		}
	};

}

TEST_SUITE("Editor.ImportSettings")
{
	TEST_CASE("asset.setImportSettings imports in the background while the editor's frames go on")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportBackground");
		const std::string asset = harness.Add("Data/Slow.stgated", { 's' });
		CHECK(GatedImporter::s_LastLevel == 0);

		GatedImporter::SetGate(false);
		auto completed = harness.Start("asset.setImportSettings", { { "asset", asset }, { "settings", { { "level", 7 } } } });
		REQUIRE_FALSE(completed->has_value());
		// The .meta holds the setting at once, in its canonical form; the import waits at the gate while frames run.
		CHECK(harness.Run("asset.info", { { "asset", asset } })["importSettings"]["Level"] == 7);
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		for (int frame = 0; frame < 30; frame++)
			harness.Frame();
		CHECK_FALSE(completed->has_value());
		CHECK(harness.Run("asset.info", { { "asset", asset } })["importing"] == true);

		GatedImporter::SetGate(true);
		const EditorCommandResult result = harness.Finish(completed);
		INFO(result.Error);
		REQUIRE(result.Success);
		CHECK(result.Value["importSettings"]["Level"] == 7);
		CHECK(result.Value["importing"] == false);
		CHECK(result.Value["importError"] == "");
		CHECK(GatedImporter::s_LastLevel == 7);
		CHECK(harness.Run("asset.info", { { "asset", asset } })["importing"] == false);

		// wait: false answers at once; the import still happens.
		const nlohmann::json started = harness.Run("asset.setImportSettings", { { "asset", asset }, { "settings", { { "Level", 3 } } }, { "wait", false } });
		CHECK(started["importing"] == true);
		REQUIRE(Tests::WaitUntil([&]()
		{
			harness.Frame();
			return harness.Run("asset.info", { { "asset", asset } })["importing"] == false;
		}, std::chrono::seconds(60)));
		CHECK(GatedImporter::s_LastLevel == 3);

		// asset.reimport imports again even though the cached import is current.
		const uint32_t imports = GatedImporter::s_ImportCount;
		const EditorCommandResult reimported = harness.Finish(harness.Start("asset.reimport", { { "asset", "Data/Slow.stgated" } }));
		CHECK(reimported.Success);
		CHECK(GatedImporter::s_ImportCount == imports + 1);
	}

	TEST_CASE("A project with imports to do opens at once and imports them while the editor's frames go on")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		// A project whose file has to be imported again when it opens: its cache is gone.
		std::filesystem::path projectDirectory;
		UUID asset = UUID::Null();
		{
			ImportHarness harness("EditorOpenImports");
			const std::optional<UUID> handle = UUID::FromString(harness.Add("Data/Slow.stgated", { 's' }));
			REQUIRE(handle);
			asset = *handle;
			projectDirectory = harness.Context.GetProject()->GetProjectDirectory();
			harness.Context.CloseProject();
		}
		REQUIRE(FileSystem::Remove(projectDirectory / ".strata" / "Cache"));

		EditorContextSpecification specification { false };
		specification.ImportInBackground = true;
		EditorContext context { specification };
		EditorCommandRegistry commands;
		GatedImporter::SetGate(false);
		std::string error;
		REQUIRE_MESSAGE(context.OpenProject(projectDirectory, &error), error); // Returns while the import waits at the gate
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		EditorAssetManager* assets = context.GetAssetManager();
		REQUIRE(assets);
		CHECK(assets->IsImportPending(asset));
		CHECK(assets->HasPendingImports());
		CHECK(assets->GetPendingImportCount() == 1);
		const EditorCommandResult status = commands.Execute(context, "editor.status", nlohmann::json::object());
		REQUIRE(status.Success);
		CHECK(status.Value["assets"]["importing"] == 1);

		// A load of the asset waits for its import (Loading, not failed) while frames run.
		CHECK(AssetManager::GetAsset<Font>(asset) == nullptr);
		for (int frame = 0; frame < 30; frame++)
		{
			context.Update(Timestep(1.0f / 60.0f));
			CHECK(assets->GetAssetState(asset) == AssetState::Loading);
		}

		// Once imported, it loads.
		GatedImporter::SetGate(true);
		REQUIRE(Tests::WaitUntil([&]()
		{
			context.Update(Timestep(1.0f / 60.0f));
			AssetManager::GetAsset<Font>(asset);
			return assets->GetAssetState(asset) == AssetState::Ready;
		}, std::chrono::seconds(60)));
		CHECK_FALSE(assets->IsImportPending(asset));
		CHECK(assets->GetPendingImportCount() == 0);
		CHECK(assets->GetImportInfo(asset).Error.empty());
	}

	TEST_CASE("Without background imports a project opens with every asset imported, and a failed first import fails its loads")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		std::filesystem::path projectDirectory;
		UUID asset = UUID::Null();
		{
			ImportHarness harness("EditorOpenImportsWaiting");
			const std::optional<UUID> handle = UUID::FromString(harness.Add("Data/Slow.stgated", { 's' }));
			REQUIRE(handle);
			asset = *handle;
			projectDirectory = harness.Context.GetProject()->GetProjectDirectory();
			harness.Context.CloseProject();
		}
		REQUIRE(FileSystem::Remove(projectDirectory / ".strata" / "Cache"));

		// Tests and scripted runs: the import is done when OpenProject returns.
		const uint32_t imports = GatedImporter::s_ImportCount;
		{
			EditorContext context { EditorContextSpecification { false } };
			std::string error;
			REQUIRE_MESSAGE(context.OpenProject(projectDirectory, &error), error);
			CHECK(GatedImporter::s_ImportCount == imports + 1);
			CHECK_FALSE(context.GetAssetManager()->HasPendingImports());
			CHECK(context.GetAssetManager()->GetImportInfo(asset).Imported);
		}

		// In the background, a first import that fails ends the loads that waited for it with its error.
		REQUIRE(FileSystem::Remove(projectDirectory / ".strata" / "Cache"));
		std::filesystem::path meta = projectDirectory / "Assets" / "Data" / "Slow.stgated";
		meta += ".meta";
		std::optional<std::string> text = FileSystem::ReadText(meta);
		REQUIRE(text);
		std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document);
		(*document)["ImportSettings"]["Fail"] = true;
		REQUIRE(FileSystem::WriteText(meta, JsonUtils::Dump(*document, 1, '\t')));
		EditorContextSpecification specification { false };
		specification.ImportInBackground = true;
		EditorContext context { specification };
		GatedImporter::SetGate(false);
		std::string error;
		REQUIRE_MESSAGE(context.OpenProject(projectDirectory, &error), error);
		EditorAssetManager* assets = context.GetAssetManager();
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		CHECK(AssetManager::GetAsset<Font>(asset) == nullptr);
		context.Update(Timestep(1.0f / 60.0f));
		CHECK(assets->GetAssetState(asset) == AssetState::Loading);
		GatedImporter::SetGate(true);
		REQUIRE(Tests::WaitUntil([&]()
		{
			context.Update(Timestep(1.0f / 60.0f));
			return assets->GetAssetState(asset) == AssetState::Failed;
		}, std::chrono::seconds(60)));
		CHECK(assets->GetAssetError(asset).find("Requested failure") != std::string::npos);
	}

	TEST_CASE("The assets pill counts the imports running in the background")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		Tests::HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f });
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorImportPill") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const std::filesystem::path external = directory.parent_path() / "External" / "Slow.stgated";
		REQUIRE(FileSystem::CreateDirectories(external.parent_path()));
		REQUIRE(FileSystem::WriteBytes(external, std::vector<uint8_t> { 's' }));
		GatedImporter::SetGate(true);
		const std::optional<UUID> asset = UUID::FromString(
			editor.Run("asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", "Data" } })["asset"].get<std::string>());
		REQUIRE(asset);
		editor.Frames(2);
		CHECK(editor.GetPillText("Status.Assets").find("importing") == std::string::npos);

		// While the import waits at the gate, the pill says so first.
		GatedImporter::SetGate(false);
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		REQUIRE(assets->ReimportAssetInBackground(*asset));
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		editor.Frames(2);
		const std::string pill = editor.GetPillText("Status.Assets");
		CHECK_MESSAGE(pill.rfind("1 importing", 0) == 0, pill);

		GatedImporter::SetGate(true);
		REQUIRE(editor.FramesUntil([&]() { return assets->GetPendingImportCount() == 0; }));
		editor.Frames(1);
		CHECK(editor.GetPillText("Status.Assets").find("importing") == std::string::npos);
	}

	TEST_CASE("A failed background import fails the command, and closing the project ends it")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportFailure");
		const std::string asset = harness.Add("Data/Slow.stgated", { 's' });

		const EditorCommandResult failed = harness.Finish(harness.Start("asset.setImportSettings", { { "asset", asset }, { "settings", { { "Fail", true } } } }));
		CHECK_FALSE(failed.Success);
		CHECK(failed.ErrorKind == EditorCommandError::Failed);
		CHECK(failed.Error == "Import failed: Requested failure");
		CHECK(harness.Run("asset.info", { { "asset", asset } })["importError"] == "Requested failure");
		CHECK(harness.Finish(harness.Start("asset.setImportSettings", { { "asset", asset }, { "settings", { { "Fail", false } } } })).Success);

		OpenGateAtEnd gate;
		GatedImporter::SetGate(false);
		auto pending = harness.Start("asset.reimport", { { "asset", asset } });
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		// Closing the project cancels the import without waiting for it: the gate stays closed, and the import stops by itself.
		const uint32_t cancelled = GatedImporter::s_CancelledCount;
		const double seconds = SecondsOf([&]() { harness.Context.CloseProject(); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Closing the project took ", seconds, " s");
		REQUIRE(Tests::WaitUntil([&]() { return GatedImporter::s_CancelledCount > cancelled; }, std::chrono::seconds(20)));
		const EditorCommandResult closed = harness.Finish(pending);
		CHECK_FALSE(closed.Success);
		CHECK(closed.Error == "The project was closed before the import finished");
	}

	TEST_CASE("Closing the project, opening another and quitting never wait for the project's imports")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenGateAtEnd gate;
		// A project with three files to import when it opens (its cache is gone), and another one to open.
		std::filesystem::path projectDirectory;
		std::vector<std::string> assets;
		{
			ImportHarness harness("EditorCloseImports");
			for (const char* name : { "Data/A.stgated", "Data/B.stgated", "Data/C.stgated" })
				assets.push_back(harness.Add(name, { 's' }));
			projectDirectory = harness.Context.GetProject()->GetProjectDirectory();
			harness.Context.CloseProject();
		}
		REQUIRE(FileSystem::Remove(projectDirectory / ".strata" / "Cache"));
		std::filesystem::path otherDirectory;
		{
			ImportHarness other("EditorCloseImportsOther");
			otherDirectory = other.Context.GetProject()->GetProjectDirectory();
		}

		EditorContextSpecification specification { false };
		specification.ImportInBackground = true;
		GatedImporter::SetGate(false);
		const uint32_t cancelled = GatedImporter::s_CancelledCount;
		const uint32_t imports = GatedImporter::s_ImportCount;
		auto openHeld = [&](EditorContext& context)
		{
			std::string error;
			REQUIRE_MESSAGE(context.OpenProject(projectDirectory, &error), error);
			REQUIRE(Tests::WaitUntil([]() { return GatedImporter::GetWaiting() == 2; }, std::chrono::seconds(20)));
			CHECK(context.GetAssetManager()->GetPendingImportCount() == 3);
		};
		auto context = std::make_unique<EditorContext>(specification);
		// project.close
		openHeld(*context);
		double seconds = SecondsOf([&]() { context->CloseProject(); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Closing the project took ", seconds, " s");
		REQUIRE(Tests::WaitUntil([&]() { return GatedImporter::s_CancelledCount == cancelled + 2; }, std::chrono::seconds(20)));

		// project.open of another project (project.openSample opens its copy the same way)
		openHeld(*context);
		std::string error;
		bool opened = false;
		seconds = SecondsOf([&]() { opened = context->OpenProject(otherDirectory, &error); });
		REQUIRE_MESSAGE(opened, error);
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Opening another project took ", seconds, " s");
		REQUIRE(Tests::WaitUntil([&]() { return GatedImporter::s_CancelledCount == cancelled + 4; }, std::chrono::seconds(20)));

		// Quitting the editor: its context goes with the project open.
		openHeld(*context);
		seconds = SecondsOf([&]() { context.reset(); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Destroying the editor's context took ", seconds, " s");
		REQUIRE(Tests::WaitUntil([&]() { return GatedImporter::s_CancelledCount == cancelled + 6; }, std::chrono::seconds(20)));
		// No held import got through: the queued third import never ran, and the cancelled ones stored nothing.
		CHECK(GatedImporter::s_ImportCount == imports);

		// The next open imports all three again.
		GatedImporter::SetGate(true);
		EditorContext reopened { EditorContextSpecification { false } };
		REQUIRE_MESSAGE(reopened.OpenProject(projectDirectory, &error), error);
		CHECK(GatedImporter::s_ImportCount == imports + 3);
		for (const std::string& asset : assets)
		{
			const std::optional<UUID> handle = UUID::FromString(asset);
			REQUIRE(handle);
			CHECK(reopened.GetAssetManager()->GetImportInfo(*handle).Imported);
		}
	}

	TEST_CASE("asset.move and asset.delete answer at once while imports run, and cancel only the imports of what they change")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenGateAtEnd gate;
		ImportHarness harness("EditorMoveImports");
		const std::string a = harness.Add("Data/A.stgated", { 'a' });
		const std::string b = harness.Add("Data/B.stgated", { 'b' });
		const std::string c = harness.Add("Data/C.stgated", { 'c' });
		const std::string material = harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"].get<std::string>();

		// Two imports run (held), a third waits for a thread.
		GatedImporter::SetGate(false);
		for (const std::string& asset : { a, b, c })
			harness.Run("asset.reimport", { { "asset", asset }, { "wait", false } });
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::GetWaiting() == 2; }, std::chrono::seconds(20)));
		const uint32_t cancelled = GatedImporter::s_CancelledCount;

		// What is not importing moves at once, cancelling nothing.
		double seconds = SecondsOf([&]() { harness.Run("asset.move", { { "asset", material }, { "path", "Materials/Moved/Red.stmat" } }); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "asset.move of the material took ", seconds, " s");
		CHECK(harness.Run("editor.status")["assets"]["importing"] == 3);

		// Moving or deleting an importing asset answers at once: its import is cancelled (a moved one runs again at the new
		// place); the others go on.
		seconds = SecondsOf([&]() { harness.Run("asset.move", { { "asset", a }, { "path", "Moved/A.stgated" } }); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "asset.move of an importing asset took ", seconds, " s");
		seconds = SecondsOf([&]() { harness.Run("asset.delete", { { "asset", b } }); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "asset.delete of an importing asset took ", seconds, " s");
		REQUIRE(Tests::WaitUntil([&]() { return GatedImporter::s_CancelledCount == cancelled + 2; }, std::chrono::seconds(20)));
		CHECK(harness.Run("asset.info", { { "asset", a } })["importing"] == true);
		CHECK(harness.Run("asset.info", { { "asset", c } })["importing"] == true);
		CHECK(harness.Run("editor.status")["assets"]["importing"] == 2);

		GatedImporter::SetGate(true);
		REQUIRE(Tests::WaitUntil([&]()
		{
			harness.Frame();
			return harness.Run("editor.status")["assets"]["importing"] == 0;
		}, std::chrono::seconds(60)));
		CHECK(GatedImporter::s_CancelledCount == cancelled + 2);
		const nlohmann::json moved = harness.Run("asset.info", { { "asset", a } });
		CHECK(moved["path"] == "Moved/A.stgated");
		CHECK(moved["importError"] == "");
		CHECK(harness.Run("asset.info", { { "asset", c } })["importError"] == "");
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "asset.info", { { "asset", b } }).Success);
		// Nothing of the deleted asset's import was stored.
		const std::filesystem::path cache = harness.Context.GetProject()->GetCacheDirectory();
		std::error_code error;
		for (std::filesystem::directory_iterator it(cache, error), end; !error && it != end; it.increment(error))
			CHECK(FileSystem::ToUTF8(it->path().filename()).rfind(b, 0) != 0);
	}

	TEST_CASE("An export waits for running imports between frames, then packs what they imported")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenGateAtEnd gate;
		ImportHarness harness("EditorExportImports");
		const std::string asset = harness.Add("Data/Slow.stgated", { 's' });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });

		GatedImporter::SetGate(false);
		harness.Run("asset.setImportSettings", { { "asset", asset }, { "settings", { { "Level", 4 } } }, { "wait", false } });
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		const std::filesystem::path output = harness.Context.GetProject()->GetProjectDirectory().parent_path() / "Build";
		auto exported = harness.Start("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } });

		// Frames go on while the export waits for the import.
		for (int frame = 0; frame < 30; frame++)
		{
			const double seconds = SecondsOf([&]() { harness.Frame(); });
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "A frame took ", seconds, " s");
		}
		CHECK_FALSE(exported->has_value());
		CHECK(harness.Run("project.exportStatus")["state"] == "WaitingForImports");

		GatedImporter::SetGate(true);
		const EditorCommandResult result = harness.Finish(exported);
		INFO(result.Error);
		REQUIRE(result.Success);
		CHECK(GatedImporter::s_LastLevel == 4);
		// The pack holds the asset as imported with the new settings.
		CHECK(FileSystem::IsRegularFile(FileSystem::FromUTF8(result.Value["assetPack"].get<std::string>())));
		CHECK(result.Value["assetCount"].get<uint32_t>() >= 2);
	}

	TEST_CASE("Import settings are checked against the importer's description and asset.info lists it")
	{
		ImportHarness harness("EditorImportValidation");
		const std::string texture = harness.Add("Textures/Brick.png", Tests::CreateSolidPNG(8, 8, 200, 100, 50));
		const std::string material = harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"].get<std::string>();
		const nlohmann::json before = harness.Run("asset.info", { { "asset", texture } })["importSettings"];

		auto fail = [&](const nlohmann::json& parameters)
		{
			const EditorCommandResult result = harness.Commands.Execute(harness.Context, "asset.setImportSettings", parameters);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			return result.Error;
		};
		CHECK(fail({ { "asset", texture }, { "settings", { { "Compresion", "Auto" } } } }).find("Unknown import setting 'Compresion'") != std::string::npos);
		CHECK(fail({ { "asset", texture }, { "settings", { { "Usage", "Albedo" } } } }).find("Color, NormalMap, Data, HDR") != std::string::npos);
		CHECK(fail({ { "asset", texture }, { "settings", { { "MaxSize", 64 }, { "Wrap", 9 } } } }).find("Wrap") != std::string::npos);
		CHECK(fail({ { "asset", texture }, { "settings", { { "MaxSize", 99999 } } } }).find("99999 is outside its range, 0 to 16384") != std::string::npos);
		CHECK(fail({ { "asset", material }, { "settings", { { "Size", 1 } } } }).find("Material assets have no import settings") != std::string::npos);
		CHECK(fail({ { "asset", "Builtin/Cube" }, { "settings", nlohmann::json::object() } }).find("not a file of the project") != std::string::npos);
		CHECK(fail({ { "asset", texture }, { "settings", nlohmann::json::object() }, { "wait", 1 } }).find("wait") != std::string::npos);
		CHECK(harness.Run("asset.info", { { "asset", texture } })["importSettings"] == before);

		// Every setting with what it accepts, for the inspector and for agents.
		const nlohmann::json info = harness.Run("asset.info", { { "asset", texture } });
		const nlohmann::json& properties = info["importSettingsProperties"];
		REQUIRE(properties.size() == 8);
		CHECK(properties[0]["Name"] == "Usage");
		CHECK(properties[0]["Options"] == nlohmann::json { "Color", "NormalMap", "Data", "HDR" });
		CHECK(properties[1]["Name"] == "Compression");
		CHECK(properties[1]["Options"] == nlohmann::json { "Auto", "Uncompressed" });
		CHECK(properties[2]["Name"] == "Quality");
		CHECK(properties[2]["Options"] == nlohmann::json { "High", "Fast" });
		CHECK(properties[3]["Name"] == "MaxSize");
		CHECK(harness.Run("asset.info", { { "asset", material } })["importSettingsProperties"].empty());
	}
}
