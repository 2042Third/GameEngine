#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Asset/GltfImporter.h>
#include <Strata/Asset/TextureImporter.h>
#include <Strata/Core/BinaryStream.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Import memory admission (EditorAssetManager::SetImportMemoryBudget): imports run while their estimates fit the budget
// together, wait without holding a thread otherwise, and one runs alone when it exceeds the budget by itself.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Imports ".sttestgated" files, whose text is their estimate in bytes ("fail <estimate>": the import fails once the gate
	// opens), and holds each import until the gate opens; it records what ran at once.
	class GatedImporter final : public AssetImporter
	{
	public:
		static inline std::atomic<bool> s_Open = false;
		static inline std::mutex s_Mutex;
		static inline uint64_t s_Running = 0;      // The estimates of the imports running now
		static inline uint32_t s_RunningCount = 0;
		static inline uint64_t s_MaxRunning = 0;
		static inline uint32_t s_MaxRunningCount = 0;
		static inline uint32_t s_Imported = 0;
		static inline bool s_LargeRanAlone = true;
		static inline bool s_LargeRunning = false;

		AssetType GetType() const override { return AssetType::Font; }
		std::vector<std::string> GetExtensions() const override { return { ".sttestgated" }; }
		uint32_t GetVersion() const override { return 1; }

		static uint64_t ParseEstimate(std::string_view text)
		{
			if (text.starts_with("fail "))
				text.remove_prefix(5);
			return std::stoull(std::string(text));
		}

		uint64_t EstimateImportMemory(std::span<const uint8_t> head, uint64_t, const nlohmann::json&) const override
		{
			return ParseEstimate(std::string_view(reinterpret_cast<const char*>(head.data()), head.size()));
		}

		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
		{
			const std::optional<std::string> text = FileSystem::ReadText(context.SourcePath);
			const uint64_t estimate = text ? ParseEstimate(*text) : 0;
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_Running += estimate;
				s_RunningCount++;
				s_MaxRunning = std::max(s_MaxRunning, s_Running);
				s_MaxRunningCount = std::max(s_MaxRunningCount, s_RunningCount);
				// An import over the budget (1,000 in the tests) runs with no other.
				if (s_LargeRunning || (estimate > 1000 && s_RunningCount > 1))
					s_LargeRanAlone = false;
				if (estimate > 1000)
					s_LargeRunning = true;
			}
			while (!s_Open.load() && !context.IsCancelled())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_Running -= estimate;
				s_RunningCount--;
				s_Imported++;
				if (estimate > 1000)
					s_LargeRunning = false;
			}
			if (text && text->starts_with("fail "))
			{
				if (outError)
					*outError = "Requested failure";
				return false;
			}
			result.Data = std::vector<uint8_t>(text ? text->begin() : std::string::const_iterator(), text ? text->end() : std::string::const_iterator());
			return true;
		}

		static void Reset()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			s_Open = false;
			s_Running = 0;
			s_RunningCount = 0;
			s_MaxRunning = 0;
			s_MaxRunningCount = 0;
			s_Imported = 0;
			s_LargeRanAlone = true;
			s_LargeRunning = false;
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

	EditorAssetManagerSpecification MakeSpecification(const std::filesystem::path& root)
	{
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = root / "Assets";
		specification.CacheDirectory = root / ".strata" / "Cache";
		specification.WatchFiles = false;
		return specification;
	}

}

TEST_SUITE("Asset.ImportAdmission")
{
	TEST_CASE("Background imports run while their estimates fit the budget, wait without holding threads, and one over it runs alone")
	{
		// Three background threads: two hold the imports that run (held at the gate), the third must stay free.
		ScopedJobSystem jobs(3, 1);
		RegisterGatedImporter();
		GatedImporter::Reset();
		const std::filesystem::path root = CreateTemporaryDirectory("ImportAdmission");
		const std::vector<uint64_t> estimates = { 600, 600, 300, 2000, 100, 400 };
		for (size_t index = 0; index < estimates.size(); index++)
		{
			// The last one fails: a failed import releases its estimate like one that succeeds.
			const std::string text = index + 1 == estimates.size() ? fmt::format("fail {}", estimates[index]) : std::to_string(estimates[index]);
			REQUIRE(FileSystem::WriteText(root / "Assets" / fmt::format("File{}.sttestgated", index), text));
		}

		EditorAssetManager manager(MakeSpecification(root));
		manager.SetImportMemoryBudget(1000);
		manager.BeginScan(ScanImports::InBackground);
		// Every import is admitted or waits: what runs fits the budget, and the threads are free.
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			const ImportMemoryStats stats = manager.GetImportMemoryStats();
			return !manager.IsScanning() && stats.Running + stats.Waiting == estimates.size();
		}, std::chrono::seconds(30)));
		ImportMemoryStats stats = manager.GetImportMemoryStats();
		CHECK(stats.Budget == 1000);
		CHECK(stats.Admitted <= 1000);
		// What runs fits the budget (or is one import alone); the rest waits, in the order the scan queued them, behind the
		// first that did not fit.
		CHECK(stats.Running >= 1);
		CHECK(stats.Running <= 2);
		CHECK((stats.Admitted <= 1000 || stats.Running == 1));
		CHECK(stats.Waiting == estimates.size() - stats.Running);
		CHECK(stats.Queued == 600 + 600 + 300 + 2000 + 100 + 400 - stats.Admitted);
		// Only the running imports hold background threads (no other job runs now), and a background job runs meanwhile on
		// a free one.
		const uint32_t running = stats.Running;
		CHECK(WaitUntil([running]() { return JobSystem::GetStats().Background.RunningJobs == running; }, std::chrono::seconds(10)));
		std::atomic<bool> ran = false;
		JobSystem::Submit([&ran]() { ran.store(true); }, JobPriority::Background);
		CHECK(WaitUntil([&]() { return ran.load(); }, std::chrono::seconds(10)));

		// Open: they all run, in admitted groups.
		GatedImporter::s_Open = true;
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			return !manager.HasPendingImports();
		}, std::chrono::seconds(60)));
		CHECK(GatedImporter::s_Imported == estimates.size());
		CHECK(GatedImporter::s_MaxRunning <= 2000);
		CHECK(GatedImporter::s_LargeRanAlone);
		stats = manager.GetImportMemoryStats();
		CHECK(stats.Admitted == 0);
		CHECK(stats.Waiting == 0);
		for (size_t index = 0; index + 1 < estimates.size(); index++)
		{
			const AssetImportInfo info = manager.GetImportInfo(manager.FindAssetByPath(fmt::format("File{}.sttestgated", index)));
			CHECK(info.Imported);
			CHECK(info.Error.empty());
		}
		const AssetImportInfo failed = manager.GetImportInfo(manager.FindAssetByPath(fmt::format("File{}.sttestgated", estimates.size() - 1)));
		CHECK(failed.Imported);
		CHECK(failed.Error.find("Requested failure") != std::string::npos);
	}

	TEST_CASE("A blocking import waits on its thread while the imports that run leave no room for it")
	{
		ScopedJobSystem jobs(2, 1);
		RegisterGatedImporter();
		GatedImporter::Reset();
		const std::filesystem::path root = CreateTemporaryDirectory("ImportAdmissionBlocking");
		REQUIRE(FileSystem::WriteText(root / "Assets" / "Running.sttestgated", "800"));
		REQUIRE(FileSystem::WriteText(root / "Assets" / "Blocking.sttestgated", "500"));
		EditorAssetManager manager(MakeSpecification(root));
		manager.SetImportMemoryBudget(1000);
		GatedImporter::s_Open = true;
		manager.BeginScan(ScanImports::InBackground);
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			return !manager.IsScanning() && !manager.HasPendingImports();
		}, std::chrono::seconds(30)));
		GatedImporter::Reset();

		// The 800 runs in the background, held at the gate; a blocking re-import of the 500 on this thread must wait for it
		// (together they would take 1,300). The gate opens a moment later: had the 500 started beside it, the importer would
		// have seen both running.
		REQUIRE(manager.ReimportAssetInBackground(manager.FindAssetByPath("Running.sttestgated")));
		REQUIRE(WaitUntil([&]()
		{
			std::scoped_lock<std::mutex> lock(GatedImporter::s_Mutex);
			return GatedImporter::s_RunningCount == 1;
		}, std::chrono::seconds(10)));
		std::thread opener([]()
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			GatedImporter::s_Open = true;
		});
		std::string error;
		const bool reimported = manager.ReimportAsset(manager.FindAssetByPath("Blocking.sttestgated"), &error);
		opener.join();
		CHECK_MESSAGE(reimported, error);
		CHECK(GatedImporter::s_MaxRunning == 800);
		CHECK(GatedImporter::s_MaxRunningCount == 1);
		CHECK(GatedImporter::s_Imported == 2);
		CHECK(WaitUntil([&]()
		{
			manager.Update();
			return !manager.HasPendingImports();
		}, std::chrono::seconds(30)));
		CHECK(manager.GetImportMemoryStats().Admitted == 0);
	}

	TEST_CASE("A larger budget starts the imports that wait for it, and imports that wait end when the manager closes")
	{
		ScopedJobSystem jobs(2, 1);
		RegisterGatedImporter();
		GatedImporter::Reset();
		const std::filesystem::path root = CreateTemporaryDirectory("ImportAdmissionBudget");
		for (int index = 0; index < 4; index++)
			REQUIRE(FileSystem::WriteText(root / "Assets" / fmt::format("File{}.sttestgated", index), "500"));
		{
			EditorAssetManager manager(MakeSpecification(root));
			manager.SetImportMemoryBudget(500);
			manager.BeginScan(ScanImports::InBackground);
			REQUIRE(WaitUntil([&]()
			{
				manager.Update();
				const ImportMemoryStats stats = manager.GetImportMemoryStats();
				return !manager.IsScanning() && stats.Running == 1 && stats.Waiting == 3;
			}, std::chrono::seconds(30)));
			manager.SetImportMemoryBudget(1500);
			CHECK(WaitUntil([&]() { return manager.GetImportMemoryStats().Running == 3; }, std::chrono::seconds(10)));
			CHECK(manager.GetImportMemoryStats().Waiting == 1);
			// Closing cancels them: the one that waits never runs, and nothing hangs.
			manager.Close();
		}
		CHECK(WaitUntil([]()
		{
			std::scoped_lock<std::mutex> lock(GatedImporter::s_Mutex);
			return GatedImporter::s_RunningCount == 0;
		}, std::chrono::seconds(10)));
		CHECK(GatedImporter::s_MaxRunningCount <= 3);
	}

	TEST_CASE("Estimates come from the source's header: textures by their pixels, glTF by their buffers, the rest by their size")
	{
		CHECK(EditorAssetManager::GetDefaultImportMemoryBudget() >= (1ull << 30));
		CHECK(EditorAssetManager::GetDefaultImportMemoryBudget() <= (4ull << 30));

		const TextureImporter texture;
		const std::vector<uint8_t> png = CreateSolidPNG(64, 32, 1, 2, 3);
		CHECK(texture.EstimateImportMemory(png, png.size(), nlohmann::json::object()) == 64ull * 32 * 20);
		const std::vector<uint8_t> garbage = { 1, 2, 3, 4 };
		CHECK(texture.EstimateImportMemory(garbage, 1000, nlohmann::json::object()) == 4000);

		// A .glb whose JSON names a buffer of 1,000,000 bytes and an image of 50,000 bytes in a buffer view.
		const std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":1000000}],"bufferViews":[{"buffer":0,"byteLength":50000}],"images":[{"bufferView":0,"mimeType":"image/png"}]})";
		BinaryWriter glb;
		glb.WriteBytes("glTF", 4);
		glb.Write(uint32_t(2));
		glb.Write(uint32_t(0));
		glb.Write(static_cast<uint32_t>(json.size()));
		glb.WriteBytes("JSON", 4);
		glb.WriteBytes(json.data(), json.size());
		const GltfImporter gltf;
		// Twice the buffer and eight times the image: 2,400,000, unless four times the source's size is more.
		CHECK(gltf.EstimateImportMemory(glb.GetData(), 100'000, nlohmann::json::object()) == 2'400'000);
		CHECK(gltf.EstimateImportMemory(glb.GetData(), 1'100'000, nlohmann::json::object()) == 4'400'000);
		// A head that does not hold the document: four times the size.
		CHECK(gltf.EstimateImportMemory(std::span<const uint8_t>(glb.GetData().data(), 30), 100'000, nlohmann::json::object()) == 400'000);
		// A .gltf held whole by its head, whose lengths are crafted to wrap a sum around: the estimate saturates.
		const std::string huge = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":18446744073709551615},{"byteLength":9223372036854775807}]})";
		const std::span<const uint8_t> hugeBytes(reinterpret_cast<const uint8_t*>(huge.data()), huge.size());
		CHECK(gltf.EstimateImportMemory(hugeBytes, huge.size(), nlohmann::json::object()) == UINT64_MAX);
	}
}
