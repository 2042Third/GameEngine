#include "Editor/DocumentSaver.h"

#include <Strata/Asset/AssetDependencies.h>
#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/AtomicFileWriter.h>
#include <Strata/Core/Hash.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/Scene.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <exception>
#include <span>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		// Counts what goes through, for the request's progress (read by the main thread while the job writes).
		class CountingOutput final : public DocumentOutput
		{
		public:
			CountingOutput(DocumentOutput& output, std::atomic<uint64_t>& total)
				: m_Output(output), m_Total(total)
			{
			}

			bool Write(std::string_view bytes) override
			{
				if (!m_Output.Write(bytes))
					return false;
				m_Total.fetch_add(bytes.size(), std::memory_order_relaxed);
				return true;
			}

			uint64_t GetBytesWritten() const override { return m_Output.GetBytesWritten(); }
		private:
			DocumentOutput& m_Output;
			std::atomic<uint64_t>& m_Total;
		};

		// What the extra data of components (script fields) references, as the asset pipeline reads it.
		void ScanExtraReferences(const ComponentInfo& info, const nlohmann::json& component, std::vector<AssetHandle>& outReferences)
		{
			const nlohmann::json snapshot = { { "Entities", nlohmann::json::array({ { { "Components", { { info.Name, component } } } } }) } };
			AssetDependencies::ScanEntities(snapshot, outReferences);
		}

		// Lets go of a snapshot nothing writes any more on a background thread: destroying a large scene takes tens of
		// milliseconds.
		void ReleaseSnapshot(Ref<const Scene>& snapshot)
		{
			if (!snapshot)
				return;
			JobSystem::Submit([released = std::move(snapshot)]() mutable { released.reset(); }, JobPriority::Background);
			snapshot = nullptr;
		}

		bool SameDocuments(const DocumentSaveRequest& a, const DocumentSaveRequest& b)
		{
			if (a.Items.size() != b.Items.size())
				return false;
			std::unordered_set<AssetHandle> assets;
			for (const DocumentSaveItem& item : a.Items)
				assets.insert(item.Asset);
			return std::all_of(b.Items.begin(), b.Items.end(), [&assets](const DocumentSaveItem& item) { return assets.contains(item.Asset); });
		}

	}

	const char* DocumentSaveStateToString(DocumentSaveState state)
	{
		switch (state)
		{
			case DocumentSaveState::Queued:     return "Queued";
			case DocumentSaveState::Writing:    return "Writing";
			case DocumentSaveState::Publishing: return "Publishing";
			case DocumentSaveState::Done:       return "Done";
			case DocumentSaveState::Failed:     return "Failed";
		}
		return "Unknown";
	}

	struct DocumentSaver::Job
	{
		struct Item
		{
			DocumentSaveItem Spec;
			Ref<DocumentWriteClaim> Claim; // Null when the item could not be claimed (Result.Error says why)
			DocumentSaveItemResult Result;
			WrittenDocument Written;       // Once committed
			bool Committed = false;
		};

		Ref<const Scene> Snapshot;
		std::vector<Item> Items;
		std::atomic<float> Progress = 0.0f;
		std::atomic<uint64_t> Bytes = 0;
		std::chrono::steady_clock::time_point Started;
		std::chrono::steady_clock::time_point Ended; // Written by the job before Done
		std::atomic<bool> Done = false;
		JobHandle Handle;

		void Run();
		void WriteItem(size_t index);
	};

	void DocumentSaver::Job::Run()
	{
		for (size_t index = 0; index < Items.size(); index++)
		{
			if (Items[index].Claim)
			{
				// Third-party code (JSON) may throw: the item fails, the others go on.
				try
				{
					WriteItem(index);
				}
				catch (const std::exception& exception)
				{
					Items[index].Result.Error = fmt::format("Writing failed: {}", exception.what());
				}
			}
			Progress.store(static_cast<float>(index + 1) / static_cast<float>(Items.size()), std::memory_order_relaxed);
		}
		// The snapshot goes here, off the main thread.
		Snapshot.reset();
		Ended = std::chrono::steady_clock::now();
		Done.store(true, std::memory_order_release);
	}

	void DocumentSaver::Job::WriteItem(size_t index)
	{
		Item& item = Items[index];
		DocumentSaveItemResult& result = item.Result;
		AtomicFileWriter file(item.Claim->GetAbsolutePath());
		if (!file.IsOpen())
		{
			result.Error = file.GetError();
			return;
		}
		CountingOutput output(file, Bytes);
		std::string error;
		uint64_t contentHash = 0;
		std::vector<AssetHandle> references;
		if (item.Spec.Document)
		{
			// Bytes made elsewhere are checked by their loader before they replace a file, as SaveNativeAsset does.
			const std::string& document = *item.Spec.Document;
			const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.data()), document.size());
			const AssetLoadFunction* loader = AssetLoaderRegistry::Find(item.Claim->GetType());
			AssetLoadData data(bytes);
			if (!loader || !(*loader)(AssetMetadata { item.Claim->GetAsset(), item.Claim->GetType(), item.Claim->GetPath() }, data, &error))
			{
				result.Error = fmt::format("Invalid {} data: {}", AssetTypeToString(item.Claim->GetType()), error);
				return;
			}
			if (!AssetDependencies::Scan(item.Claim->GetType(), bytes, references, &error))
			{
				result.Error = fmt::format("Its references could not be read: {}", error);
				return;
			}
			if (!output.Write(document))
			{
				result.Error = file.GetError();
				return;
			}
			contentHash = Hash::FNV1a(bytes);
			result.Sha256 = Sha256::Hash(bytes);
		}
		else
		{
			if (!Snapshot)
			{
				result.Error = "The request has no snapshot to write";
				return;
			}
			SceneWriteOptions options = item.Spec.Write;
			const float first = static_cast<float>(index) / static_cast<float>(Items.size());
			const float share = 1.0f / static_cast<float>(Items.size());
			options.Progress = [this, first, share](float done) { Progress.store(first + share * done, std::memory_order_relaxed); };
			if (!options.ExtraReferences)
				options.ExtraReferences = ScanExtraReferences;
			SceneWriteResult written;
			if (!SceneDocumentWriter::Write(*Snapshot, output, options, &written, &error))
			{
				result.Error = file.GetError().empty() ? error : file.GetError();
				return;
			}
			references = std::move(written.References);
			contentHash = written.ContentHash;
			result.Sha256 = written.Sha256;
			result.Entities = written.Entities;
		}
		result.References = references;
		if (!item.Claim->Commit(file, contentHash, std::move(references), item.Written, &error))
		{
			result.Error = error;
			return;
		}
		item.Written.Claim = item.Claim;
		item.Written.Reload = item.Spec.Reload;
		result.Bytes = item.Written.Size;
		item.Committed = true;
	}

	DocumentSaver::~DocumentSaver()
	{
		FinishAll();
	}

	uint64_t DocumentSaver::Submit(DocumentSaveRequest request, Completion onComplete, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return uint64_t(0);
		};

		if (!m_Assets)
			return fail("No project is open");
		if (request.Items.empty())
			return fail("A save needs at least one document");
		std::unordered_set<AssetHandle> assets;
		for (const DocumentSaveItem& item : request.Items)
		{
			if (!assets.insert(item.Asset).second)
				return fail(fmt::format("The document {} is listed twice", item.Asset.ToString()));
			const AssetImporter* importer = m_Assets->GetImporter(item.Asset);
			const bool native = importer && importer->StoresSourceDirectly();
			if (!native && m_Assets->GetReservedPath(item.Asset).empty())
				return fail(fmt::format("{} is no engine asset of the project and was not reserved (ReserveNativeAsset)", item.Asset.ToString()));
			if (!item.Document && !request.Snapshot)
				return fail(fmt::format("The document {} has neither bytes of its own nor a snapshot to be written from", item.Asset.ToString()));
		}

		const uint64_t id = m_NextID++;
		for (const Scope<Pending>& queued : m_Queue)
		{
			if (!SameDocuments(queued->Request, request))
				continue;
			// The queued request never started: the newer snapshot is written for both.
			ReleaseSnapshot(queued->Request.Snapshot);
			queued->Request = std::move(request);
			queued->Assets = m_Assets;
			queued->IDs.push_back(id);
			queued->Completions.push_back(std::move(onComplete));
			return id;
		}
		Scope<Pending> pending = CreateScope<Pending>();
		pending->IDs.push_back(id);
		pending->Completions.push_back(std::move(onComplete));
		pending->Request = std::move(request);
		pending->Assets = m_Assets;
		m_Queue.push_back(std::move(pending));
		// Started at the next update (or at once by FinishAll), so that a caller's later submissions can still replace it.
		return id;
	}

	void DocumentSaver::Start(Scope<Pending> pending)
	{
		Ref<Job> job = CreateRef<Job>();
		job->Started = std::chrono::steady_clock::now();
		job->Snapshot = std::move(pending->Request.Snapshot);
		pending->Request.Snapshot = nullptr;
		job->Items.reserve(pending->Request.Items.size());
		for (const DocumentSaveItem& item : pending->Request.Items)
		{
			Job::Item& jobItem = job->Items.emplace_back();
			jobItem.Spec = item;
			jobItem.Result.Asset = item.Asset;
			std::string error;
			jobItem.Claim = pending->Assets->ClaimDocumentWrite(item.Asset, &error);
			jobItem.Result.Path = jobItem.Claim ? jobItem.Claim->GetPath() : item.NewPath;
			if (!jobItem.Claim)
				jobItem.Result.Error = error;
		}
		pending->Running = job;
		job->Handle = JobSystem::Submit([job]() { job->Run(); }, JobPriority::Background);
		m_Running = std::move(pending);
	}

	bool DocumentSaver::Collect()
	{
		if (!m_Running)
			return true;
		Job& job = *m_Running->Running;
		if (!job.Done.load(std::memory_order_acquire))
			return false;
		JobSystem::Wait(job.Handle);

		// Published in one step: each written document becomes its asset's state; what failed leaves its asset as it was.
		std::string errors;
		for (Job::Item& item : job.Items)
		{
			if (item.Committed)
			{
				m_Running->Assets->PublishWrittenDocument(item.Written);
			}
			else
			{
				if (item.Claim && item.Claim->IsNewAsset())
					m_Running->Assets->ReleaseNativeAssetReservation(item.Spec.Asset);
				errors += fmt::format("{}'{}': {}", errors.empty() ? "" : "; ", item.Result.Path.empty() ? item.Spec.Asset.ToString() : item.Result.Path,
					item.Result.Error);
			}
			item.Claim.reset();
			item.Written.Claim.reset();
		}

		const Scope<Pending> ended = std::move(m_Running);
		std::vector<DocumentSaveStatus> statuses;
		for (const uint64_t id : ended->IDs)
		{
			DocumentSaveStatus status = MakeStatus(*ended, id);
			status.State = errors.empty() ? DocumentSaveState::Done : DocumentSaveState::Failed;
			status.Progress = 1.0f;
			status.Error = errors.empty() ? std::string() : fmt::format("Not saved: {}", errors);
			for (const Job::Item& item : job.Items)
				status.Items.push_back(item.Result);
			if (!errors.empty())
				ST_ERROR("Saving '{}' failed: {}", status.Label, errors);
			Keep(status);
			statuses.push_back(std::move(status));
		}
		// Completions may submit requests: they queue, and the next update starts them.
		for (size_t index = 0; index < statuses.size(); index++)
		{
			if (ended->Completions[index])
				ended->Completions[index](statuses[index]);
		}
		return true;
	}

	void DocumentSaver::Update()
	{
		Collect();
		if (!m_Running && !m_Queue.empty())
		{
			Scope<Pending> next = std::move(m_Queue.front());
			m_Queue.pop_front();
			Start(std::move(next));
		}
	}

	void DocumentSaver::FinishAll()
	{
		while (IsBusy())
		{
			if (m_Running)
			{
				JobSystem::Wait(m_Running->Running->Handle);
				Collect();
				continue;
			}
			Scope<Pending> next = std::move(m_Queue.front());
			m_Queue.pop_front();
			Start(std::move(next));
		}
	}

	DocumentSaveStatus DocumentSaver::MakeStatus(const Pending& pending, uint64_t id) const
	{
		DocumentSaveStatus status;
		status.ID = id;
		status.WrittenID = pending.IDs.back();
		status.Label = pending.Request.Label;
		status.SavePoint = pending.Request.SavePoint;
		if (const Ref<Job>& job = pending.Running)
		{
			const bool done = job->Done.load(std::memory_order_acquire);
			status.State = done ? DocumentSaveState::Publishing : DocumentSaveState::Writing;
			status.Progress = job->Progress.load(std::memory_order_relaxed);
			status.Bytes = job->Bytes.load(std::memory_order_relaxed);
			status.Seconds = std::chrono::duration<double>((done ? job->Ended : std::chrono::steady_clock::now()) - job->Started).count();
		}
		return status;
	}

	void DocumentSaver::Keep(const DocumentSaveStatus& status)
	{
		m_Ended.push_back(status);
		while (m_Ended.size() > c_KeptStatuses)
			m_Ended.pop_front();
	}

	std::optional<DocumentSaveStatus> DocumentSaver::GetStatus(uint64_t id) const
	{
		const auto has = [id](const Pending& pending) { return std::find(pending.IDs.begin(), pending.IDs.end(), id) != pending.IDs.end(); };
		if (m_Running && has(*m_Running))
			return MakeStatus(*m_Running, id);
		for (const Scope<Pending>& queued : m_Queue)
		{
			if (has(*queued))
				return MakeStatus(*queued, id);
		}
		for (auto it = m_Ended.rbegin(); it != m_Ended.rend(); ++it)
		{
			if (it->ID == id)
				return *it;
		}
		return std::nullopt;
	}

	std::vector<DocumentSaveStatus> DocumentSaver::GetActiveStatuses() const
	{
		std::vector<DocumentSaveStatus> statuses;
		if (m_Running)
			statuses.push_back(MakeStatus(*m_Running, m_Running->IDs.back()));
		for (const Scope<Pending>& queued : m_Queue)
			statuses.push_back(MakeStatus(*queued, queued->IDs.back()));
		return statuses;
	}

	bool DocumentSaver::IsSaving(AssetHandle asset) const
	{
		const auto writes = [asset](const Pending& pending)
		{
			return std::any_of(pending.Request.Items.begin(), pending.Request.Items.end(), [asset](const DocumentSaveItem& item) { return item.Asset == asset; });
		};
		if (m_Running && writes(*m_Running))
			return true;
		return std::any_of(m_Queue.begin(), m_Queue.end(), [&writes](const Scope<Pending>& pending) { return writes(*pending); });
	}

}
