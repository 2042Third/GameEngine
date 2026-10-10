#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
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
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

using namespace Strata;

namespace
{

	// Imports ".stgated" files as fonts (the embedded default font), but only once the test opens its gate, so a test can
	// hold an import while the editor's frames go on. Settings: {"Level": 0..10, "Fail": bool}.
	class GatedImporter final : public AssetImporter
	{
	public:
		static inline std::mutex s_Mutex;
		static inline std::condition_variable s_Condition;
		static inline bool s_GateOpen = true;
		static inline uint32_t s_Waiting = 0;
		static inline std::atomic<uint32_t> s_ImportCount = 0;
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
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_Waiting > 0;
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
				const bool opened = s_Condition.wait_for(lock, std::chrono::seconds(60), []() { return s_GateOpen; });
				s_Waiting--;
				if (!opened)
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

		GatedImporter::SetGate(false);
		auto pending = harness.Start("asset.reimport", { { "asset", asset } });
		REQUIRE(Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)));
		// Closing the project waits for the import, so the gate opens on another thread meanwhile.
		std::thread opener([]()
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			GatedImporter::SetGate(true);
		});
		harness.Context.CloseProject();
		opener.join();
		const EditorCommandResult closed = harness.Finish(pending);
		CHECK_FALSE(closed.Success);
		CHECK(closed.Error == "The project was closed before the import finished");
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
		REQUIRE(properties.size() == 6);
		CHECK(properties[0]["Name"] == "Usage");
		CHECK(properties[0]["Options"] == nlohmann::json { "Color", "NormalMap", "Data", "HDR" });
		CHECK(properties[1]["Name"] == "MaxSize");
		CHECK(harness.Run("asset.info", { { "asset", material } })["importSettingsProperties"].empty());
	}
}
