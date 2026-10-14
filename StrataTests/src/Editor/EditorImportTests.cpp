#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorNotifications.h"
#include "Editor/HarnessEditor.h"
#include "Renderer/GPUTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Font.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Scene/Components.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

	// Imports ".stheld" files as fonts (the embedded default font). Its files may name others (ReadsExternalFiles), and
	// asking which waits at a gate the test holds: a folder import's copy asks right after copying such a file, so while the
	// gate is closed the file lies complete in the project and the copy has not finished.
	class HeldCopyImporter final : public AssetImporter
	{
	public:
		static inline std::mutex s_Mutex;
		static inline std::condition_variable s_Condition;
		static inline bool s_GateOpen = true;
		static inline uint32_t s_Waiting = 0;
		static inline std::atomic<uint32_t> s_ImportCount = 0;

		static void SetGate(bool open)
		{
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_GateOpen = open;
			}
			s_Condition.notify_all();
		}

		static bool IsWaiting()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_Waiting > 0;
		}

		AssetType GetType() const override { return AssetType::Font; }
		std::vector<std::string> GetExtensions() const override { return { ".stheld" }; }
		uint32_t GetVersion() const override { return 1; }
		bool ReadsExternalFiles() const override { return true; }

		bool FindExternalFiles(std::span<const uint8_t>, std::vector<std::string>&, std::string*) const override
		{
			std::unique_lock<std::mutex> lock(s_Mutex);
			s_Waiting++;
			s_Condition.wait_for(lock, std::chrono::seconds(60), []() { return s_GateOpen; });
			s_Waiting--;
			return true;
		}

		bool Import(const ImportContext&, ImportResult& result, std::string*) const override
		{
			s_ImportCount++;
			result.Data = Font::GetDefault()->GetData();
			return true;
		}
	};

	void RegisterHeldCopyImporter()
	{
		static bool s_Registered = false;
		if (!s_Registered)
		{
			AssetImporterRegistry::Register(CreateScope<HeldCopyImporter>());
			s_Registered = true;
		}
	}

	// Opens the held copy's gate when the test ends, also when it fails, before the job system shuts down (declare it after
	// the jobs).
	struct OpenHeldCopyAtEnd
	{
		~OpenHeldCopyAtEnd()
		{
			HeldCopyImporter::SetGate(true);
		}
	};

	// Opens the gate when the test ends, also when it fails, before the job system shuts down (declare it after the jobs).
	struct OpenGateAtEnd
	{
		~OpenGateAtEnd()
		{
			GatedImporter::SetGate(true);
		}
	};

	// A glTF model of a textured quad: its buffer as `bufferUri` (written beside the model, or embedded as a data URI when
	// empty) and its base color image as `imageUri` (written relative to the model when given), as an exporter writes
	// them: the audit's fixture is Quad.gltf with Quad.bin and QuadColor.png beside it.
	void WriteQuadModel(const std::filesystem::path& model, const std::string& bufferUri, const std::string& imageUri)
	{
		std::vector<uint8_t> buffer;
		auto append = [&buffer](const auto& values)
		{
			const auto* bytes = reinterpret_cast<const uint8_t*>(values.data());
			buffer.insert(buffer.end(), bytes, bytes + values.size() * sizeof(values[0]));
		};
		append(std::vector<float> { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0 });
		append(std::vector<float> { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 });
		append(std::vector<float> { 0, 1, 1, 1, 1, 0, 0, 0 });
		append(std::vector<uint16_t> { 0, 1, 2, 0, 2, 3 });
		nlohmann::json document = {
			{ "asset", { { "version", "2.0" } } },
			{ "scenes", nlohmann::json::array({ { { "nodes", { 0 } } } }) },
			{ "nodes", nlohmann::json::array({ { { "name", "Quad" }, { "mesh", 0 } } }) },
			{ "meshes", nlohmann::json::array({ { { "primitives", nlohmann::json::array({ { { "attributes", { { "POSITION", 0 }, { "NORMAL", 1 }, { "TEXCOORD_0", 2 } } },
				{ "indices", 3 }, { "material", 0 } } }) } } }) },
			{ "materials", nlohmann::json::array({ { { "name", "Painted" }, { "pbrMetallicRoughness", { { "baseColorTexture", { { "index", 0 } } } } } } }) },
			{ "textures", nlohmann::json::array({ { { "source", 0 } } }) },
			{ "images", nlohmann::json::array({ { { "uri", imageUri } } }) },
			{ "buffers", nlohmann::json::array({ { { "byteLength", buffer.size() } } }) },
			{ "bufferViews", nlohmann::json::array({
				{ { "buffer", 0 }, { "byteOffset", 0 }, { "byteLength", 48 } }, { { "buffer", 0 }, { "byteOffset", 48 }, { "byteLength", 48 } },
				{ { "buffer", 0 }, { "byteOffset", 96 }, { "byteLength", 32 } }, { { "buffer", 0 }, { "byteOffset", 128 }, { "byteLength", 12 } } }) },
			{ "accessors", nlohmann::json::array({
				{ { "bufferView", 0 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 1, 1, 0 } } },
				{ { "bufferView", 1 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC3" } },
				{ { "bufferView", 2 }, { "componentType", 5126 }, { "count", 4 }, { "type", "VEC2" } },
				{ { "bufferView", 3 }, { "componentType", 5123 }, { "count", 6 }, { "type", "SCALAR" } } }) } };
		if (bufferUri.empty())
		{
			std::string base64 = "data:application/octet-stream;base64,";
			static constexpr char c_Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			for (size_t index = 0; index < buffer.size(); index += 3)
			{
				const uint32_t chunk = (static_cast<uint32_t>(buffer[index]) << 16) | (index + 1 < buffer.size() ? buffer[index + 1] << 8 : 0)
					| (index + 2 < buffer.size() ? buffer[index + 2] : 0);
				base64 += c_Alphabet[(chunk >> 18) & 63];
				base64 += c_Alphabet[(chunk >> 12) & 63];
				base64 += index + 1 < buffer.size() ? c_Alphabet[(chunk >> 6) & 63] : '=';
				base64 += index + 2 < buffer.size() ? c_Alphabet[chunk & 63] : '=';
			}
			document["buffers"][0]["uri"] = base64;
		}
		else
		{
			document["buffers"][0]["uri"] = bufferUri;
			REQUIRE(FileSystem::WriteBytes(model.parent_path() / FileSystem::FromUTF8(bufferUri), buffer));
		}
		REQUIRE(FileSystem::WriteText(model, JsonUtils::Dump(document, 1, '\t')));
		REQUIRE(FileSystem::WriteBytes((model.parent_path() / FileSystem::FromUTF8(imageUri)).lexically_normal(), Tests::CreateSolidPNG(8, 8, 200, 60, 30)));
	}

	// A PNG that stb_image refuses with a message quoting the bytes of a chunk type that are no text (as the audit's
	// "END\xEF\xBF\xBD PNG chunk not known").
	std::vector<uint8_t> CreatePNGWithUnknownChunk()
	{
		std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		const uint8_t header[] = { 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 1, 0, 0, 0, 1, 8, 2, 0, 0, 0, 0, 0, 0, 0 };
		png.insert(png.end(), std::begin(header), std::end(header));
		// Image data first: reading the header (stbi_info) stops there, so only decoding meets the chunk after it.
		const uint8_t data[] = { 0, 0, 0, 1, 'I', 'D', 'A', 'T', 0, 0, 0, 0, 0 };
		png.insert(png.end(), std::begin(data), std::end(data));
		// A critical chunk (bit 5 of its first byte clear) whose type is no ASCII.
		const uint8_t unknown[] = { 0, 0, 0, 0, 0x8F, 'E', 'N', 'D', 0, 0, 0, 0 };
		png.insert(png.end(), std::begin(unknown), std::end(unknown));
		return png;
	}

	// How many log entries after `sequence` contain the text.
	size_t CountLogEntries(uint64_t sequence, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(sequence))
		{
			if (entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

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
		EditorContext Context;
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::filesystem::path Assets;

		// `watchFiles`: the editor's file watcher runs, as in the editor (tests leave it off: its timing is the file system's).
		explicit ImportHarness(const std::string& name, bool watchFiles = false)
			: Context(EditorContextSpecification { watchFiles })
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
			const EditorCommandResult imported = Finish(Start("asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", directory } }));
			INFO(imported.Error);
			REQUIRE(imported.Success);
			return imported.Value["asset"].get<std::string>();
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

	TEST_CASE("Opening a project before the first frame waits for the background imports its start scene uses")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenGateAtEnd gate;
		// A project whose start scene draws a text in a font that has to be imported again when it opens: its cache is gone.
		std::filesystem::path projectDirectory;
		UUID font = UUID::Null();
		AssetHandle startScene = UUID::Null();
		{
			ImportHarness harness("EditorOpenStartSceneImports");
			const std::string handle = harness.Add("Data/Slow.stgated", { 's' });
			const std::optional<UUID> parsed = UUID::FromString(handle);
			REQUIRE(parsed);
			font = *parsed;
			harness.Run("entity.create", { { "name", "Title" }, { "components", { { "Text", { { "Text", "Hello" }, { "Font", handle } } } } } });
			harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
			harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
			startScene = harness.Context.GetSceneHandle();
			projectDirectory = harness.Context.GetProject()->GetProjectDirectory();
			harness.Context.CloseProject();
		}
		REQUIRE(startScene.IsValid());
		REQUIRE(FileSystem::Remove(projectDirectory / ".strata" / "Cache"));

		// A headless editor opens its project and start scene before its first frame (EditorContext::OpenProject) while the
		// font imports in the background: the wait applies the import once it is done, so the scene opens as soon as it is in
		// instead of when the wait gives up.
		EditorContextSpecification specification { false };
		specification.ImportInBackground = true;
		EditorContext context { specification };
		GatedImporter::SetGate(false);
		std::thread opener([]()
		{
			if (Tests::WaitUntil([]() { return GatedImporter::IsWaiting(); }, std::chrono::seconds(20)))
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
			GatedImporter::SetGate(true);
		});
		const auto start = std::chrono::steady_clock::now();
		std::string error;
		const bool opened = context.OpenProject(projectDirectory, &error);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		opener.join();
		REQUIRE_MESSAGE(opened, error);
		CHECK_MESSAGE(seconds < 30.0, "Opening took ", seconds, " s");
		const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
		INFO(status.Error);
		CHECK(status.StartScene);
		CHECK(status.State == SceneLoadState::Ready);
		CHECK(context.GetSceneHandle() == startScene);
		CHECK(context.GetEditScene()->FindEntityByName("Title"));
		EditorAssetManager* assets = context.GetAssetManager();
		REQUIRE(assets);
		CHECK(assets->GetImportInfo(font).Imported);
		CHECK(assets->GetAssetState(font) == AssetState::Ready);
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
			editor.RunToCompletion("asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", "Data" } })["asset"].get<std::string>());
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
		REQUIRE(properties.size() == 9);
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

TEST_SUITE("Editor.Import")
{
	TEST_CASE("A glTF model with its buffer and image beside it imports textured, its files copied with it")
	{
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportGltfExternal");
		const std::filesystem::path external = harness.Assets.parent_path() / "Downloads";
		WriteQuadModel(external / "Quad.gltf", "Quad.bin", "QuadColor.png");

		EditorCommandResult result = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(external / "Quad.gltf") }, { "directory", "Models" } }));
		INFO(result.Error);
		REQUIRE(result.Success);
		CHECK(result.Value["path"] == "Models/Quad/Quad.gltf");
		REQUIRE(result.Value["items"].size() == 1);
		nlohmann::json& item = result.Value["items"][0];
		CHECK(item["path"] == "Models/Quad/Quad.gltf");
		CHECK_FALSE(item.contains("error"));
		CHECK(FileSystem::IsRegularFile(harness.Assets / "Models" / "Quad" / "Quad.bin"));
		CHECK(FileSystem::IsRegularFile(harness.Assets / "Models" / "Quad" / "QuadColor.png"));
		// The model first, then its image, which is an asset of its own.
		REQUIRE(item["assets"].size() == 2);
		CHECK(item["assets"][0]["type"] == "Model");
		CHECK(item["assets"][1]["path"] == "Models/Quad/QuadColor.png");

		nlohmann::json info = harness.Run("asset.info", { { "asset", "Models/Quad/Quad.gltf" } });
		CHECK(info["importError"] == "");
		for (const nlohmann::json& warning : info["importWarnings"])
			CHECK(warning.get<std::string>().find("skipped") == std::string::npos);
		const nlohmann::json& subAssets = info["subAssets"];
		CHECK(std::any_of(subAssets.begin(), subAssets.end(), [](const nlohmann::json& subAsset) { return subAsset["type"] == "Texture"; }));

		// Again: a folder of its own.
		EditorCommandResult again = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(external / "Quad.gltf") }, { "directory", "Models" } }));
		REQUIRE(again.Success);
		CHECK(again.Value["path"] == "Models/Quad (1)/Quad.gltf");
	}

	TEST_CASE("A model whose image lies in ../Textures imports textured, keeping that layout")
	{
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportGltfParent");
		const std::filesystem::path external = harness.Assets.parent_path() / "Kit";
		WriteQuadModel(external / "Models" / "Crate.gltf", "", "../Textures/Wood.png");

		EditorCommandResult result = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(external / "Models" / "Crate.gltf") },
			{ "directory", "Props" } }));
		INFO(result.Error);
		REQUIRE(result.Success);
		CHECK(result.Value["path"] == "Props/Crate/Models/Crate.gltf");
		CHECK(FileSystem::IsRegularFile(harness.Assets / "Props" / "Crate" / "Textures" / "Wood.png"));
		nlohmann::json info = harness.Run("asset.info", { { "asset", "Props/Crate/Models/Crate.gltf" } });
		CHECK(info["importError"] == "");
		for (const nlohmann::json& warning : info["importWarnings"])
			CHECK(warning.get<std::string>().find("skipped") == std::string::npos);
		const nlohmann::json& subAssets = info["subAssets"];
		CHECK(std::any_of(subAssets.begin(), subAssets.end(), [](const nlohmann::json& subAsset) { return subAsset["type"] == "Texture"; }));

		// A file three folders up is too far; a URL is no file.
		WriteQuadModel(external / "Deep" / "A" / "B" / "Far.gltf", "", "../../../Textures/Far.png");
		const uint64_t sequence = Log::GetBuffer().GetLatestSequence();
		EditorCommandResult far = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(external / "Deep" / "A" / "B" / "Far.gltf") } }));
		CHECK_FALSE(far.Success);
		CHECK(far.Error.find("more than 2 folders above") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Far"));
		// Logged once, like failed imports: the editor's UI runs the command without reading its answer.
		CHECK(CountLogEntries(sequence, "more than 2 folders above") == 1);
	}

	TEST_CASE("A folder imports with its tree, its .meta files and what its models read")
	{
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportFolder");
		const std::filesystem::path pack = harness.Assets.parent_path() / "Pack";
		REQUIRE(FileSystem::WriteBytes(pack / "Textures" / "Stone.png", Tests::CreateSolidPNG(4, 4, 90, 90, 90)));
		REQUIRE(FileSystem::WriteText(pack / "Textures" / "Stone.png.meta", "{ \"Handle\": \"5A5A5A5A5A5A5A5A\", \"ImportSettings\": { \"MaxSize\": 2 } }"));
		REQUIRE(FileSystem::WriteBytes(pack / "Sounds" / "Beep.wav", Tests::CreateSineWav(0.1f)));
		WriteQuadModel(pack / "Models" / "Quad.gltf", "Quad.bin", "QuadColor.png");
		REQUIRE(FileSystem::WriteText(pack / "Notes.txt", "Not an asset"));
		REQUIRE(FileSystem::WriteText(pack / ".git" / "config", "Hidden"));

		EditorCommandResult result = harness.Finish(harness.Start("asset.import", { { "directories", { FileSystem::ToUTF8(pack) } }, { "directory", "Imported" } }));
		INFO(result.Error);
		REQUIRE(result.Success);
		nlohmann::json& item = result.Value["items"][0];
		CHECK(item["folder"] == true);
		CHECK(item["path"] == "Imported/Pack");
		CHECK(item["assets"].size() == 4); // The model, its image, the texture and the sound
		CHECK(FileSystem::IsRegularFile(harness.Assets / "Imported" / "Pack" / "Models" / "Quad.bin"));
		CHECK(FileSystem::IsRegularFile(harness.Assets / "Imported" / "Pack" / "Sounds" / "Beep.wav"));
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Imported" / "Pack" / "Notes.txt"));
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Imported" / "Pack" / ".git"));
		// The .meta came along: its handle and settings.
		nlohmann::json stone = harness.Run("asset.info", { { "asset", "Imported/Pack/Textures/Stone.png" } });
		CHECK(stone["asset"] == "5A5A5A5A5A5A5A5A");
		CHECK(stone["importSettings"]["MaxSize"] == 2);
		CHECK(harness.Context.GetAssetManager()->HasFolder("Imported/Pack/Sounds"));
		CHECK(harness.Run("asset.info", { { "asset", "Imported/Pack/Models/Quad.gltf" } })["importError"] == "");
	}

	TEST_CASE("A failed import is a failure that leaves no file behind and is logged once")
	{
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportFailure");
		const std::filesystem::path broken = harness.Assets.parent_path() / "Downloads" / "Broken.png";
		REQUIRE(FileSystem::WriteBytes(broken, std::vector<uint8_t> { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e' }));

		const uint64_t sequence = Log::GetBuffer().GetLatestSequence();
		EditorCommandResult failed = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(broken) }, { "directory", "Textures" } }));
		CHECK_FALSE(failed.Success);
		CHECK(failed.ErrorKind == EditorCommandError::Failed);
		CHECK(failed.Error.find("Broken.png") != std::string::npos);
		REQUIRE(failed.ErrorData.is_object());
		CHECK(failed.ErrorData["items"][0]["rolledBack"] == true);
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Textures" / "Broken.png"));
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Textures" / "Broken.png.meta"));
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Textures")); // The folder it made goes too
		CHECK(harness.Run("asset.list", { { "includeBuiltin", false } })["assets"].empty());
		CHECK(CountLogEntries(sequence, "Broken.png") == 1);
		// Its caller reports the failure: no toast tells of an import nobody waited for.
		CHECK_FALSE(harness.Context.GetAssetManager()->HasImportFailures());
		const std::deque<EditorNotification>& toasts = harness.Context.GetNotifications().GetActive();
		CHECK(std::none_of(toasts.begin(), toasts.end(), [](const EditorNotification& toast) { return toast.Group == "AssetImport"; }));

		// Kept on request, with its error.
		EditorCommandResult kept = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(broken) }, { "keepOnFailure", true } }));
		CHECK_FALSE(kept.Success);
		CHECK(kept.ErrorData["items"][0]["rolledBack"] == false);
		CHECK(FileSystem::IsRegularFile(harness.Assets / "Broken.png"));
		CHECK(harness.Run("asset.info", { { "asset", "Broken.png" } })["importError"].get<std::string>().find("Broken.png") != std::string::npos);

		// Nothing to import is refused at once.
		CHECK(harness.Commands.Execute(harness.Context, "asset.import", { { "file", FileSystem::ToUTF8(broken.parent_path() / "Missing.png") } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(harness.Context, "asset.import", { { "file", FileSystem::ToUTF8(harness.Assets.parent_path() / "Game.stproj") } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(harness.Context, "asset.import", nlohmann::json::object()).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("Decoder messages that quote bytes of a file reach errors and logs as valid UTF-8")
	{
		Tests::ScopedJobSystem jobs(2, 2);
		ImportHarness harness("EditorImportUTF8");
		const std::filesystem::path png = harness.Assets.parent_path() / "Downloads" / "Dropped.png";
		REQUIRE(FileSystem::WriteBytes(png, CreatePNGWithUnknownChunk()));
		const uint64_t sequence = Log::GetBuffer().GetLatestSequence();
		EditorCommandResult failed = harness.Finish(harness.Start("asset.import", { { "file", FileSystem::ToUTF8(png) }, { "keepOnFailure", true } }));
		CHECK_FALSE(failed.Success);
		CHECK(failed.Error.find("PNG chunk not known") != std::string::npos);
		CHECK(failed.Error.find("\xEF\xBF\xBD" "END") != std::string::npos);
		CHECK(StringUtils::MakeValidUTF8(failed.Error) == failed.Error);
		const std::string importError = harness.Run("asset.info", { { "asset", "Dropped.png" } })["importError"].get<std::string>();
		CHECK(StringUtils::MakeValidUTF8(importError) == importError);
		bool logged = false;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(sequence))
		{
			if (entry.Message.find("Dropped.png") == std::string::npos)
				continue;
			logged = true;
			CHECK(StringUtils::MakeValidUTF8(entry.Message) == entry.Message);
		}
		CHECK(logged);
	}

	TEST_CASE("asset.import copies and imports while the editor's frames go on")
	{
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenGateAtEnd gate;
		ImportHarness harness("EditorImportAsync");
		const std::filesystem::path external = harness.Assets.parent_path() / "External" / "Held.stgated";
		REQUIRE(FileSystem::CreateDirectories(external.parent_path()));
		REQUIRE(FileSystem::WriteBytes(external, std::vector<uint8_t> { 'h' }));

		GatedImporter::SetGate(false);
		auto completed = harness.Start("asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", "Data" } });
		REQUIRE(Tests::WaitUntil([&]()
		{
			harness.Frame();
			return GatedImporter::IsWaiting();
		}, std::chrono::seconds(20)));
		// Frames go on while the import waits; the asset is registered and says it is importing.
		for (int frame = 0; frame < 30; frame++)
		{
			const double seconds = SecondsOf([&]() { harness.Frame(); });
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "A frame took ", seconds, " s");
		}
		CHECK_FALSE(completed->has_value());
		CHECK(harness.Run("asset.info", { { "asset", "Data/Held.stgated" } })["importing"] == true);
		CHECK(harness.Context.GetAssetManager()->GetExternalImportCount() == 1);

		GatedImporter::SetGate(true);
		const EditorCommandResult result = harness.Finish(completed);
		INFO(result.Error);
		REQUIRE(result.Success);
		CHECK(result.Value["path"] == "Data/Held.stgated");
		CHECK(harness.Context.GetAssetManager()->GetExternalImportCount() == 0);

		// Closing the project ends an import that waits: the command fails, the editor never waits.
		GatedImporter::SetGate(false);
		auto pending = harness.Start("asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", "Data" } });
		REQUIRE(Tests::WaitUntil([&]()
		{
			harness.Frame();
			return GatedImporter::IsWaiting();
		}, std::chrono::seconds(20)));
		const double seconds = SecondsOf([&]() { harness.Context.CloseProject(); });
		CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Closing the project took ", seconds, " s");
		const EditorCommandResult closed = harness.Finish(pending);
		CHECK_FALSE(closed.Success);
		CHECK(closed.Error.find("closed") != std::string::npos);
	}

	TEST_CASE("Imports copy one at a time, and the file watcher leaves what they copy alone until it is complete")
	{
		// The editor watches its asset directory. A large file being copied can look settled to the watcher (its directory
		// entry changes only when the copy closes it), which would import it half written; and two copies beside each other
		// could pick the same free name.
		RegisterGatedImporter();
		RegisterHeldCopyImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenHeldCopyAtEnd gate;
		ImportHarness harness("EditorImportWatched", true);
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const std::filesystem::path pack = harness.Assets.parent_path() / "Pack";
		REQUIRE(FileSystem::WriteBytes(pack / "A.stheld", std::vector<uint8_t> { 'a' }));
		REQUIRE(FileSystem::WriteBytes(pack / "B.stheld", std::vector<uint8_t> { 'b' }));
		const std::filesystem::path single = harness.Assets.parent_path() / "Single" / "D.stgated";
		REQUIRE(FileSystem::WriteBytes(single, std::vector<uint8_t> { 'd' }));
		const uint32_t imports = HeldCopyImporter::s_ImportCount;

		// The folder's copy stops after A, which lies complete in the project; the second import waits for that copy.
		HeldCopyImporter::SetGate(false);
		auto folder = harness.Start("asset.import", { { "directories", { FileSystem::ToUTF8(pack) } }, { "directory", "Imported" } });
		auto file = harness.Start("asset.import", { { "file", FileSystem::ToUTF8(single) }, { "directory", "Imported" } });
		REQUIRE(Tests::WaitUntil([&]()
		{
			harness.Frame();
			return HeldCopyImporter::IsWaiting();
		}, std::chrono::seconds(20)));
		const std::filesystem::path copied = harness.Assets / "Imported" / "Pack" / "A.stheld";
		REQUIRE(FileSystem::IsRegularFile(copied));

		// A file written into the project after it: once the watcher reported that one, it has reported A too.
		const std::filesystem::path control = harness.Assets / "Control.stgated";
		REQUIRE(FileSystem::WriteBytes(control, std::vector<uint8_t> { 'c' }));
		REQUIRE(Tests::WaitUntil([&]()
		{
			harness.Frame();
			return assets.FindAssetByAbsolutePath(control).IsValid();
		}, std::chrono::seconds(20)));
		CHECK_FALSE(assets.FindAssetByAbsolutePath(copied).IsValid());
		CHECK_FALSE(FileSystem::Exists(harness.Assets / "Imported" / "D.stgated"));
		CHECK_FALSE(folder->has_value());
		CHECK_FALSE(file->has_value());

		// The copies go on: each file becomes an asset once, imported once.
		HeldCopyImporter::SetGate(true);
		EditorCommandResult folderResult = harness.Finish(folder);
		INFO(folderResult.Error);
		REQUIRE(folderResult.Success);
		CHECK(folderResult.Value["items"][0]["assets"].size() == 2);
		EditorCommandResult fileResult = harness.Finish(file);
		INFO(fileResult.Error);
		REQUIRE(fileResult.Success);
		CHECK(fileResult.Value["path"] == "Imported/D.stgated");
		CHECK(assets.FindAssetByAbsolutePath(copied).IsValid());
		CHECK(HeldCopyImporter::s_ImportCount == imports + 2);
	}
}

TEST_SUITE("GPU.Editor.ImportSettings")
{
	TEST_CASE("A start scene opened before the first frame is prepared for the viewport without waiting for imports it does not use")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		RegisterGatedImporter();
		Tests::ScopedJobSystem jobs(2, 2);
		OpenGateAtEnd gate;
		// A project whose start scene draws cubes, with an asset no scene uses that has to be imported again when it opens.
		std::filesystem::path projectDirectory;
		UUID unused = UUID::Null();
		// More than a frame's share of preparation, also in optimized builds.
		const uint32_t cubes = RenderScene::c_PrepareChunk * 12;
		{
			ImportHarness harness("EditorPrepareWhileImporting");
			const std::optional<UUID> parsed = UUID::FromString(harness.Add("Data/Unused.stgated", { 's' }));
			REQUIRE(parsed);
			unused = *parsed;
			Scene& scene = *harness.Context.GetEditScene();
			for (uint32_t index = 0; index < cubes; index++)
			{
				Entity cube = scene.CreateEntity("Cube");
				cube.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index % 64) * 2.0f, 0.0f, static_cast<float>(index / 64) * 2.0f);
				cube.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
			}
			harness.Run("scene.saveAs", { { "path", "Scenes/Cubes.stscene" } });
			harness.Run("project.setStartScene", { { "scene", "Scenes/Cubes.stscene" } });
			projectDirectory = harness.Context.GetProject()->GetProjectDirectory();
			harness.Context.CloseProject();
		}
		REQUIRE(FileSystem::Remove(projectDirectory / ".strata" / "Cache"));

		// The editor opens its project and start scene before its first frame (EditorContext::OpenProject) while the unused
		// asset's import is held: the scene opens, its render proxies built, without waiting for that import (released after
		// 15 s, so that a wait shows as a slow open rather than a hang).
		EditorContextSpecification specification { false };
		specification.ImportInBackground = true;
		EditorContext context { specification };
		GatedImporter::SetGate(false);
		std::thread opener([]()
		{
			std::this_thread::sleep_for(std::chrono::seconds(15));
			GatedImporter::SetGate(true);
		});
		const auto start = std::chrono::steady_clock::now();
		std::string error;
		const bool opened = context.OpenProject(projectDirectory, &error);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		const bool stillImporting = context.GetAssetManager() && context.GetAssetManager()->IsImportPending(unused);
		opener.join();
		REQUIRE_MESSAGE(opened, error);
		CHECK_MESSAGE(seconds < 10.0, "Opening took ", seconds, " s");
		CHECK(stillImporting);
		const RenderScene* renderScene = RenderScene::Find(*context.GetEditScene());
		REQUIRE(renderScene);
		CHECK(renderScene->GetPendingCount() == 0);
		CHECK(renderScene->GetStats().Proxies == cubes);
		context.CloseProject();
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
