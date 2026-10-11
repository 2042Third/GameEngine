#include <doctest/doctest.h>

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/Material.h"
#include "TestHelpers.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// What the imports of HeldImporter did.
	struct HeldImportCounters
	{
		uint32_t Started = 0;   // Imports that reached the gate
		uint32_t Held = 0;      // Imports waiting at the gate now
		uint32_t Cancelled = 0; // Imports that noticed their cancellation at the gate
		uint32_t Finished = 0;  // Imports that returned their data
	};

	// Imports ".stheld" files as the embedded default font, with a copy of it as a sub-asset. A slow import on purpose: an
	// import that can be cancelled (a background import of the asset manager) waits at a gate until the test opens it or
	// until the import is cancelled, which it notices like a long import does, by checking its flag as it goes; imports on
	// the calling thread (a blocking scan, ReimportAsset) pass at once. Settings: {"Level": n} (recorded by the imports that
	// finish) and {"IgnoreCancel": true} (the import waits for the gate only, and finishes although it was cancelled).
	class HeldImporter final : public AssetImporter
	{
	public:
		using Counters = HeldImportCounters;

		// Never wait longer, so that a regression fails instead of hanging the tests.
		static constexpr std::chrono::seconds c_GateTimeout { 30 };

		static void Reset()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			s_GateOpen = true;
			s_Counters = {};
			s_LastLevel = -1;
			s_ResolvedAfterCancel = UUID::Null();
			s_ReadAfterCancel = false;
		}

		static void SetGate(bool open)
		{
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_GateOpen = open;
			}
			s_Condition.notify_all();
		}

		static Counters GetCounters()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_Counters;
		}

		// Waits until the counters meet the condition (false after the timeout).
		static bool WaitFor(const std::function<bool(const Counters&)>& condition)
		{
			std::unique_lock<std::mutex> lock(s_Mutex);
			return s_Condition.wait_for(lock, c_GateTimeout, [&condition]() { return condition(s_Counters); });
		}

		static int64_t GetLastLevel()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_LastLevel;
		}

		// What the last cancelled import found when it looked at the project once more: the asset its own source resolves
		// to, and whether it could read its source.
		static AssetHandle GetResolvedAfterCancel()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_ResolvedAfterCancel;
		}

		static bool GetReadAfterCancel()
		{
			std::scoped_lock<std::mutex> lock(s_Mutex);
			return s_ReadAfterCancel;
		}

		AssetType GetType() const override { return AssetType::Font; }
		std::vector<std::string> GetExtensions() const override { return { ".stheld" }; }
		uint32_t GetVersion() const override { return 1; }

		nlohmann::json GetDefaultSettings(const std::filesystem::path&) const override
		{
			return { { "Level", 0 }, { "IgnoreCancel", false } };
		}

		bool Import(const ImportContext& context, ImportResult& result, std::string* outError) const override
		{
			if (!ReadImportSource(context))
			{
				*outError = "cannot read the file";
				return false;
			}
			if (context.Cancel && !Hold(context, outError))
				return false;

			result.Data = Font::GetDefault()->GetData();
			ImportedSubAsset copy;
			copy.Key = "Copy";
			copy.Type = AssetType::Font;
			copy.Data = result.Data;
			result.SubAssets.push_back(std::move(copy));
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_Counters.Finished++;
				s_LastLevel = JsonUtils::GetInt(context.Settings, "Level", -1);
			}
			s_Condition.notify_all();
			return true;
		}
	private:
		static bool Hold(const ImportContext& context, std::string* outError)
		{
			const bool ignoreCancel = JsonUtils::GetBool(context.Settings, "IgnoreCancel", false);
			bool cancelled = false;
			{
				std::unique_lock<std::mutex> lock(s_Mutex);
				s_Counters.Started++;
				s_Counters.Held++;
				s_Condition.notify_all();
				const auto deadline = std::chrono::steady_clock::now() + c_GateTimeout;
				while (!s_GateOpen && (ignoreCancel || !context.IsCancelled()) && std::chrono::steady_clock::now() < deadline)
					s_Condition.wait_for(lock, std::chrono::milliseconds(1));
				s_Counters.Held--;
				cancelled = !ignoreCancel && context.IsCancelled();
				if (!cancelled && !s_GateOpen)
				{
					s_Condition.notify_all();
					*outError = "The test never opened the gate";
					return false;
				}
			}
			if (!cancelled)
			{
				s_Condition.notify_all();
				return true;
			}

			// A cancelled import that looks at the project once more finds nothing: no reads, and no lookups once the manager
			// closed (it may be gone).
			const AssetHandle resolved = context.ResolveAsset ? context.ResolveAsset(context.SourcePath) : UUID::Null();
			const bool read = ReadImportSource(context).has_value();
			{
				std::scoped_lock<std::mutex> lock(s_Mutex);
				s_Counters.Cancelled++;
				s_ResolvedAfterCancel = resolved;
				s_ReadAfterCancel = read;
			}
			s_Condition.notify_all();
			*outError = "cancelled";
			return false;
		}

		static inline std::mutex s_Mutex;
		static inline std::condition_variable s_Condition;
		static inline bool s_GateOpen = true;
		static inline Counters s_Counters;
		static inline int64_t s_LastLevel = -1;
		static inline AssetHandle s_ResolvedAfterCancel = UUID::Null();
		static inline bool s_ReadAfterCancel = false;
	};

	void RegisterHeldImporter()
	{
		static bool s_Registered = false;
		if (!s_Registered)
		{
			AssetImporterRegistry::Register(CreateScope<HeldImporter>());
			s_Registered = true;
		}
		HeldImporter::Reset();
	}

	// The job system with this many background threads (imports run there), shut down at the end: every job has ended then.
	class ScopedImportJobs
	{
	public:
		explicit ScopedImportJobs(uint32_t backgroundThreads)
		{
			REQUIRE_FALSE(JobSystem::IsInitialized());
			JobSystemSpecification specification;
			specification.WorkerThreadCount = 2;
			specification.IOThreadCount = 1;
			specification.BackgroundThreadCount = backgroundThreads;
			JobSystem::Init(specification);
		}

		~ScopedImportJobs()
		{
			JobSystem::Shutdown();
		}

		ScopedImportJobs(const ScopedImportJobs&) = delete;
		ScopedImportJobs& operator=(const ScopedImportJobs&) = delete;
	};

	// Opens the gate when the test ends, also when it fails, before the job system shuts down (declare it after the jobs).
	struct OpenGateAtEnd
	{
		~OpenGateAtEnd()
		{
			HeldImporter::SetGate(true);
		}
	};

	struct HeldProject
	{
		std::filesystem::path Assets;
		std::filesystem::path Cache;

		explicit HeldProject(const std::string& name)
		{
			const std::filesystem::path root = Tests::CreateTemporaryDirectory(name);
			Assets = root / "Assets";
			Cache = root / ".strata" / "Cache";
			REQUIRE(FileSystem::CreateDirectories(Assets));
		}

		void Write(const std::string& relativePath, const std::string& text) const
		{
			REQUIRE(FileSystem::WriteText(Assets / FileSystem::FromUTF8(relativePath), text));
		}

		// Background imports with ScanImports::InBackground; with Wait every import is done when it returns.
		Ref<EditorAssetManager> Open(ScanImports imports) const
		{
			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = Assets;
			specification.CacheDirectory = Cache;
			specification.WatchFiles = false;
			Ref<EditorAssetManager> manager = CreateRef<EditorAssetManager>(specification);
			manager->Scan(imports);
			return manager;
		}

		// The cache files of an asset and of its sub-asset: data, import record and files an import writes before publishing.
		std::vector<std::string> GetCacheFiles(AssetHandle handle) const
		{
			const std::string own = handle.ToString();
			const std::string copy = DeriveSubAssetHandle(handle, "Copy").ToString();
			std::vector<std::string> files;
			std::error_code error;
			for (std::filesystem::directory_iterator it(Cache, error), end; !error && it != end; it.increment(error))
			{
				const std::string name = FileSystem::ToUTF8(it->path().filename());
				if (name.rfind(own, 0) == 0 || name.rfind(copy, 0) == 0)
					files.push_back(name);
			}
			return files;
		}

		uint32_t CountStagedFiles() const
		{
			uint32_t count = 0;
			std::error_code error;
			for (std::filesystem::directory_iterator it(Cache, error), end; !error && it != end; it.increment(error))
				count += FileSystem::ToUTF8(it->path().filename()).find(".staged") != std::string::npos ? 1 : 0;
			return count;
		}
	};

	double SecondsOf(const std::function<void()>& function)
	{
		const auto start = std::chrono::steady_clock::now();
		function();
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}

	// What an operation that never waits for an import may take, generous for loaded machines: waiting for a held import
	// takes HeldImporter::c_GateTimeout.
	constexpr double c_AtOnceSeconds = 2.0;

	bool WaitForImport(EditorAssetManager& manager, AssetHandle handle)
	{
		return Tests::WaitUntil([&]()
		{
			manager.Update();
			return !manager.IsImportPending(handle);
		}, std::chrono::seconds(30));
	}

	std::string ReadRecordSource(const HeldProject& project, AssetHandle handle)
	{
		const std::optional<std::string> text = FileSystem::ReadText(project.Cache / (handle.ToString() + ".import"));
		const std::optional<nlohmann::json> record = text ? JsonUtils::Parse(*text) : std::nullopt;
		return record ? JsonUtils::GetString(*record, "Source") : std::string();
	}

}

TEST_SUITE("Asset.ImportCancellation")
{
	TEST_CASE("Closing the manager cancels its imports at once: queued ones never start, running ones store nothing")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelClose");
		const std::vector<std::string> paths = { "Data/A.stheld", "Data/B.stheld", "Data/C.stheld" };
		for (const std::string& path : paths)
			project.Write(path, path);

		std::vector<AssetHandle> handles;
		{
			// One background thread: one import runs, the others wait in the queue.
			ScopedImportJobs jobs(1);
			OpenGateAtEnd gate;
			HeldImporter::SetGate(false);
			Ref<EditorAssetManager> manager = project.Open(ScanImports::InBackground);
			for (const std::string& path : paths)
			{
				handles.push_back(manager->FindAssetByPath(path));
				REQUIRE(handles.back().IsValid());
			}
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Held == 1; }));
			CHECK(manager->GetPendingImportCount() == 3);

			// Destroying the manager (closing the project, opening another, quitting) does not wait for the held import.
			const double seconds = SecondsOf([&]() { manager.reset(); });
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Destroying the manager took ", seconds, " s");

			// The running import notices its cancellation on its own; looking again, it finds nothing of the project.
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Cancelled == 1; }));
			CHECK_FALSE(HeldImporter::GetResolvedAfterCancel().IsValid());
			CHECK_FALSE(HeldImporter::GetReadAfterCancel());
		}
		// Every job has ended with the job system: the queued imports never started, and nothing was stored.
		const HeldImporter::Counters counters = HeldImporter::GetCounters();
		CHECK(counters.Started == 1);
		CHECK(counters.Finished == 0);
		for (AssetHandle handle : handles)
			CHECK(project.GetCacheFiles(handle).empty());
		CHECK(project.CountStagedFiles() == 0);

		// The next open imports them all.
		Ref<EditorAssetManager> reopened = project.Open(ScanImports::Wait);
		for (AssetHandle handle : handles)
		{
			const AssetImportInfo info = reopened->GetImportInfo(handle);
			CHECK(info.Imported);
			CHECK(info.Error.empty());
			CHECK(reopened->IsHandleValid(DeriveSubAssetHandle(handle, "Copy")));
		}
		CHECK(HeldImporter::GetCounters().Finished == 3);
	}

	TEST_CASE("A cancelled import stores nothing, also when its importer finishes after the cancellation")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelIgnored");
		project.Write("Stubborn.stheld", "s");
		project.Write("Stubborn.stheld.meta", JsonUtils::Dump({ { "Handle", "00000000000A11CE" }, { "ImportSettings", { { "IgnoreCancel", true } } } }));
		const AssetHandle handle = UUID(0xA11CE);
		{
			ScopedImportJobs jobs(1);
			OpenGateAtEnd gate;
			HeldImporter::SetGate(false);
			Ref<EditorAssetManager> manager = project.Open(ScanImports::InBackground);
			REQUIRE(manager->FindAssetByPath("Stubborn.stheld") == handle);
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Held == 1; }));
			CHECK(manager->HasPendingImports());

			const double seconds = SecondsOf([&]() { manager->Close(); });
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Close took ", seconds, " s");
			CHECK_FALSE(manager->HasPendingImports());
			CHECK_FALSE(manager->IsImportPending(handle));
			CHECK(manager->GetPendingImportCount() == 0);

			// The importer ignores the cancellation and returns its data: none of it is stored or applied.
			HeldImporter::SetGate(true);
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Finished == 1; }));
			manager->WaitForImports(); // Nothing to wait for: the import is not wanted any more
			manager->Update();
			CHECK_FALSE(manager->GetImportInfo(handle).Imported);
			CHECK_FALSE(manager->IsHandleValid(DeriveSubAssetHandle(handle, "Copy")));
		}
		CHECK(project.GetCacheFiles(handle).empty());
		CHECK(project.CountStagedFiles() == 0);
	}

	TEST_CASE("Moving an asset cancels only its own import, which runs again at the new place")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelMove");
		project.Write("Data/A.stheld", "a");
		project.Write("Data/B.stheld", "b");
		REQUIRE(FileSystem::WriteText(project.Assets / "Materials" / "Red.stmat", JsonUtils::Dump(Material::Create()->Serialize())));
		{
			// Two background threads: both imports run at once.
			ScopedImportJobs jobs(2);
			OpenGateAtEnd gate;
			HeldImporter::SetGate(false);
			Ref<EditorAssetManager> manager = project.Open(ScanImports::InBackground);
			const AssetHandle a = manager->FindAssetByPath("Data/A.stheld");
			const AssetHandle b = manager->FindAssetByPath("Data/B.stheld");
			const AssetHandle red = manager->FindAssetByPath("Materials/Red.stmat");
			REQUIRE(a.IsValid());
			REQUIRE(b.IsValid());
			REQUIRE(red.IsValid());
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Held == 2; }));

			// An asset that is not importing moves at once, and no import is cancelled.
			std::string error;
			bool moved = false;
			double seconds = SecondsOf([&]() { moved = manager->MoveAsset(red, "Materials/Moved/Red.stmat", &error); });
			REQUIRE_MESSAGE(moved, error);
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Moving the material took ", seconds, " s");
			CHECK(manager->GetPendingImportCount() == 2);
			CHECK(HeldImporter::GetCounters().Cancelled == 0);

			// The importing asset moves at once too: its import is cancelled and runs again at the new place, the other goes on.
			seconds = SecondsOf([&]() { moved = manager->MoveAsset(a, "Moved/A.stheld", &error); });
			REQUIRE_MESSAGE(moved, error);
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Moving the importing asset took ", seconds, " s");
			CHECK(manager->IsImportPending(a));
			CHECK(manager->IsImportPending(b));
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Cancelled == 1 && counters.Held == 2; }));
			CHECK(HeldImporter::GetCounters().Started == 3);

			HeldImporter::SetGate(true);
			REQUIRE(WaitForImport(*manager, a));
			REQUIRE(WaitForImport(*manager, b));
			const HeldImporter::Counters counters = HeldImporter::GetCounters();
			CHECK(counters.Cancelled == 1);
			CHECK(counters.Finished == 2);
			for (AssetHandle handle : { a, b })
			{
				CHECK(manager->GetImportInfo(handle).Imported);
				CHECK(manager->GetImportInfo(handle).Error.empty());
			}
			// The import that stored A's data read it at its new place.
			CHECK(ReadRecordSource(project, a) == "Moved/A.stheld");
			const std::optional<AssetMetadata> copy = manager->GetMetadata(DeriveSubAssetHandle(a, "Copy"));
			REQUIRE(copy);
			CHECK(copy->Path == "Moved/A.stheld");
		}
		CHECK(project.CountStagedFiles() == 0);
	}

	TEST_CASE("Deleting an asset cancels its import, which stores nothing, and the other imports go on")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelDelete");
		project.Write("Data/A.stheld", "a");
		project.Write("Data/B.stheld", "b");
		AssetHandle a;
		AssetHandle b;
		{
			ScopedImportJobs jobs(2);
			OpenGateAtEnd gate;
			HeldImporter::SetGate(false);
			Ref<EditorAssetManager> manager = project.Open(ScanImports::InBackground);
			a = manager->FindAssetByPath("Data/A.stheld");
			b = manager->FindAssetByPath("Data/B.stheld");
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Held == 2; }));

			std::string error;
			bool deleted = false;
			const double seconds = SecondsOf([&]() { deleted = manager->DeleteAsset(a, &error); });
			REQUIRE_MESSAGE(deleted, error);
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Deleting the importing asset took ", seconds, " s");
			CHECK_FALSE(manager->IsHandleValid(a));
			CHECK(manager->GetPendingImportCount() == 1);
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Cancelled == 1; }));

			HeldImporter::SetGate(true);
			REQUIRE(WaitForImport(*manager, b));
			CHECK(manager->GetImportInfo(b).Error.empty());
			CHECK(HeldImporter::GetCounters().Finished == 1);
		}
		CHECK(project.GetCacheFiles(a).empty());
		CHECK_FALSE(project.GetCacheFiles(b).empty());
		CHECK(project.CountStagedFiles() == 0);
		CHECK_FALSE(FileSystem::Exists(project.Assets / "Data" / "A.stheld"));
		CHECK_FALSE(FileSystem::Exists(project.Assets / "Data" / "A.stheld.meta"));
	}

	TEST_CASE("Re-importing on the calling thread cancels only the asset's own background import")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelReimport");
		project.Write("Data/A.stheld", "a");
		project.Write("Data/B.stheld", "b");
		{
			ScopedImportJobs jobs(2);
			OpenGateAtEnd gate;
			HeldImporter::SetGate(false);
			Ref<EditorAssetManager> manager = project.Open(ScanImports::InBackground);
			const AssetHandle a = manager->FindAssetByPath("Data/A.stheld");
			const AssetHandle b = manager->FindAssetByPath("Data/B.stheld");
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Held == 2; }));

			// The import on the calling thread passes the gate; it neither waits for B's import nor for A's.
			std::string error;
			bool imported = false;
			const double seconds = SecondsOf([&]() { imported = manager->ReimportAsset(a, &error); });
			REQUIRE_MESSAGE(imported, error);
			CHECK_MESSAGE(seconds < c_AtOnceSeconds, "Re-importing took ", seconds, " s");
			CHECK_FALSE(manager->IsImportPending(a));
			CHECK(manager->GetImportInfo(a).Imported);
			CHECK(manager->IsImportPending(b));
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Cancelled == 1; }));

			HeldImporter::SetGate(true);
			REQUIRE(WaitForImport(*manager, b));
			const HeldImporter::Counters counters = HeldImporter::GetCounters();
			CHECK(counters.Cancelled == 1);
			CHECK(counters.Finished == 2); // A on the calling thread, and B
			CHECK(manager->GetImportInfo(b).Error.empty());
			CHECK(manager->IsHandleValid(DeriveSubAssetHandle(a, "Copy")));
		}
		CHECK(project.CountStagedFiles() == 0);
	}

	TEST_CASE("An import that replaced one which stored its result reloads the asset, also when it finds that result current")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelReplacedReload");
		project.Write("Data/A.stheld", "a");
		{
			ScopedImportJobs jobs(1);
			OpenGateAtEnd gate;
			Ref<EditorAssetManager> manager = project.Open(ScanImports::Wait);
			const AssetHandle a = manager->FindAssetByPath("Data/A.stheld");
			REQUIRE(manager->LoadAssetSync(a));
			const uint32_t finished = HeldImporter::GetCounters().Finished;
			auto readSettingsHash = [&]()
			{
				const std::optional<std::string> text = FileSystem::ReadText(project.Cache / (a.ToString() + ".import"));
				const std::optional<nlohmann::json> record = text ? JsonUtils::Parse(*text) : std::nullopt;
				return record ? JsonUtils::GetString(*record, "SettingsHash") : std::string();
			};
			const std::string settingsHash = readSettingsHash();
			REQUIRE_FALSE(settingsHash.empty());

			// A background import with new settings stores its result, and the asset moves before Update applies it: the import
			// that replaces it finds that result current.
			std::string error;
			REQUIRE_MESSAGE(manager->SetImportSettingsInBackground(a, { { "Level", 3 } }, &error), error);
			REQUIRE(Tests::WaitUntil([&]() { return readSettingsHash() != settingsHash; }, std::chrono::seconds(30)));
			const uint64_t revision = manager->GetSourceRevisions(a).Current;
			REQUIRE_MESSAGE(manager->MoveAsset(a, "Moved/A.stheld", &error), error);
			REQUIRE(WaitForImport(*manager, a));
			CHECK(HeldImporter::GetCounters().Finished == finished + 1); // The replacing import did not import again
			CHECK(HeldImporter::GetLastLevel() == 3);
			// What loaded the earlier result is reloaded.
			CHECK(manager->GetSourceRevisions(a).Current != revision);
		}
	}

	TEST_CASE("New import settings replace the import that is running instead of following it")
	{
		RegisterHeldImporter();
		HeldProject project("ImportCancelSettings");
		project.Write("Data/A.stheld", "a");
		{
			ScopedImportJobs jobs(1);
			OpenGateAtEnd gate;
			HeldImporter::SetGate(false);
			Ref<EditorAssetManager> manager = project.Open(ScanImports::InBackground);
			const AssetHandle a = manager->FindAssetByPath("Data/A.stheld");
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Held == 1; }));

			std::string error;
			REQUIRE_MESSAGE(manager->SetImportSettingsInBackground(a, { { "Level", 5 } }, &error), error);
			CHECK(manager->IsImportPending(a));
			CHECK(manager->GetPendingImportCount() == 1);
			// The running import (with the old settings) is cancelled, and the new one starts in its place.
			REQUIRE(HeldImporter::WaitFor([](const HeldImporter::Counters& counters) { return counters.Cancelled == 1 && counters.Held == 1; }));

			HeldImporter::SetGate(true);
			REQUIRE(WaitForImport(*manager, a));
			CHECK(HeldImporter::GetCounters().Finished == 1);
			CHECK(HeldImporter::GetLastLevel() == 5);
			CHECK(manager->GetImportInfo(a).Error.empty());
		}
		CHECK(project.CountStagedFiles() == 0);
	}
}
