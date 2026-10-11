#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetUploadContext.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The contract between AssetManagerBase and the assets it finalizes (AssetFinalizeContext), driven without a GPU: a fake
// upload context records the batches, and fake assets "upload" in steps the way the renderer's textures and meshes do.
namespace
{

	constexpr uint64_t c_MB = 1000 * 1000;

	// Records what the manager asks of an upload context.
	class FakeUploadContext final : public AssetUploadContext
	{
	public:
		void Begin() override
		{
			if (m_Open++ == 0)
				Begins++;
		}

		void Submit() override
		{
			REQUIRE(m_Open > 0);
			if (--m_Open == 0)
			{
				Submits++;
				SubmittedSteps += m_BatchSteps;
				m_BatchSteps = 0;
			}
		}

		bool EndFrame(uint64_t uploadedBytes) override
		{
			EndFrames.push_back(uploadedBytes);
			return ReleaseOnEndFrame;
		}

		bool IsOpen() const { return m_Open > 0; }
		void RecordStep() { m_BatchSteps++; }

		uint32_t Begins = 0;
		uint32_t Submits = 0;
		uint32_t SubmittedSteps = 0;      // Steps of batches that were submitted
		std::vector<uint64_t> EndFrames;  // The uploaded bytes each EndFrame was told
		bool ReleaseOnEndFrame = false;
	private:
		uint32_t m_Open = 0;
		uint32_t m_BatchSteps = 0;
	};

	// One FinalizeOnMainThread call as the asset saw it.
	struct FinalizeCall
	{
		AssetHandle Handle;
		uint64_t Frame = 0;           // The manager's frame
		bool FirstOfFrame = false;
		uint64_t UploadBudget = 0;
		uint64_t UploadedBefore = 0;  // *UploadedBytes when the call began
		uint32_t Steps = 0;           // Steps the call took
		bool HadUpload = false;       // It was given the manager's upload context, with a batch open
	};

	// An asset that uploads `steps` steps of `stepBytes` like a GPU asset: a call's first step goes whatever the upload budget
	// says, the frame's first step whatever the deadline says; further steps stop at the budget and the deadline.
	class SteppedAsset final : public Asset
	{
	public:
		SteppedAsset(uint32_t steps, uint64_t stepBytes, std::vector<FinalizeCall>& calls)
			: m_Steps(steps), m_StepBytes(stepBytes), m_Calls(calls)
		{
		}

		AssetType GetType() const override { return c_FakeAssetType; }
		AssetMemoryUsage GetMemoryUsage() const override { return AssetMemoryUsage { 0, m_Done * m_StepBytes, 0 }; }
		AssetMemoryUsage GetFinalizedMemoryUsage() const override { return AssetMemoryUsage { 0, m_Steps * m_StepBytes, 0 }; }

		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override
		{
			auto* upload = dynamic_cast<FakeUploadContext*>(context.Upload);
			FinalizeCall call;
			call.Handle = Handle;
			call.Frame = context.Manager ? context.Manager->GetFrameIndex() : 0;
			call.FirstOfFrame = context.FirstOfFrame;
			call.UploadBudget = context.UploadBudget;
			call.UploadedBefore = context.UploadedBytes ? *context.UploadedBytes : 0;
			call.HadUpload = upload && upload->IsOpen();

			uint64_t uploaded = 0;
			while (m_Done < m_Steps)
			{
				const bool first = call.Steps == 0;
				const bool overBudget = !first && uploaded + m_StepBytes > context.UploadBudget;
				const bool overTime = !(first && context.FirstOfFrame) && std::chrono::steady_clock::now() >= context.Deadline;
				if (overBudget || overTime)
					break;
				if (upload)
					upload->RecordStep();
				m_Done++;
				call.Steps++;
				uploaded += m_StepBytes;
			}
			if (context.UploadedBytes)
				*context.UploadedBytes += uploaded;
			m_Calls.push_back(call);
			return m_Done < m_Steps ? AssetFinalizeResult::Pending : AssetFinalizeResult::Done;
		}
	private:
		uint64_t m_Steps;
		uint64_t m_StepBytes;
		uint64_t m_Done = 0;
		std::vector<FinalizeCall>& m_Calls;
	};

	// An asset whose uploads run on a queue beside the frame's work: it is AwaitingGPU for its first `waits` calls that allow
	// it (AssetFinalizeContext::MayWait), then done; a call that does not allow it waits itself and finishes.
	class AwaitingAsset final : public Asset
	{
	public:
		AwaitingAsset(uint32_t waits, std::vector<FinalizeCall>& calls)
			: m_Waits(waits), m_Calls(calls)
		{
		}

		AssetType GetType() const override { return c_FakeAssetType; }

		AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override
		{
			FinalizeCall call;
			call.Handle = Handle;
			call.Frame = context.Manager ? context.Manager->GetFrameIndex() : 0;
			call.FirstOfFrame = context.FirstOfFrame;
			m_Calls.push_back(call);
			if (context.MayWait && m_Waits > 0)
			{
				m_Waits--;
				return AssetFinalizeResult::AwaitingGPU;
			}
			return AssetFinalizeResult::Done;
		}
	private:
		uint32_t m_Waits;
		std::vector<FinalizeCall>& m_Calls;
	};

	// Loads c_FakeAssetType as stepped assets while it lives: the stored form is (steps, step bytes); (0, waits) is an asset
	// awaiting the GPU for that many calls.
	class ScopedSteppedLoader
	{
	public:
		explicit ScopedSteppedLoader(std::vector<FinalizeCall>& calls)
		{
			const AssetLoadFunction* previous = AssetLoaderRegistry::Find(c_FakeAssetType);
			REQUIRE(previous);
			m_Previous = *previous;
			AssetLoaderRegistry::Register(c_FakeAssetType, [&calls](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
			{
				uint64_t description[2] = {};
				if (data.size() != sizeof(description))
				{
					if (outError)
						*outError = "Not a stepped asset";
					return nullptr;
				}
				std::memcpy(description, data.data(), sizeof(description));
				if (description[0] == 0)
					return CreateRef<AwaitingAsset>(static_cast<uint32_t>(description[1]), calls);
				return CreateRef<SteppedAsset>(static_cast<uint32_t>(description[0]), description[1], calls);
			});
		}

		~ScopedSteppedLoader()
		{
			AssetLoaderRegistry::Register(c_FakeAssetType, m_Previous);
		}

		ScopedSteppedLoader(const ScopedSteppedLoader&) = delete;
		ScopedSteppedLoader& operator=(const ScopedSteppedLoader&) = delete;
	private:
		AssetLoadFunction m_Previous;
	};

	std::vector<uint8_t> EncodeStepped(uint64_t steps, uint64_t stepBytes)
	{
		const uint64_t description[2] = { steps, stepBytes };
		std::vector<uint8_t> data(sizeof(description));
		std::memcpy(data.data(), description, sizeof(description));
		return data;
	}

	// Updates until every listed asset is Ready (at most `maxFrames` updates); returns the updates it took.
	uint32_t UpdateUntilReady(AssetManagerBase& manager, const std::vector<AssetHandle>& handles, uint32_t maxFrames)
	{
		for (uint32_t frame = 1; frame <= maxFrames; frame++)
		{
			manager.Update();
			if (std::all_of(handles.begin(), handles.end(), [&](AssetHandle handle) { return manager.GetAssetState(handle) == AssetState::Ready; }))
				return frame;
		}
		return maxFrames + 1;
	}

}

TEST_SUITE("Asset.Finalize")
{
	TEST_CASE("Finalization records into the manager's upload context, one batch per update, within the frame's upload budget")
	{
		std::vector<FinalizeCall> calls;
		ScopedSteppedLoader loader(calls);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const Ref<FakeUploadContext> upload = CreateRef<FakeUploadContext>();
		manager->SetUploadContext(upload);

		// Three assets of three 1 MB steps, two MB per frame: a call's first step goes whatever the budget, the steps after it
		// stop at the budget, and once a frame's budget is used up no further call starts.
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.UploadBytesPerFrame = 2 * c_MB;
		budgets.FinalizeMsPerFrame = 60000.0f; // Only the byte budget limits these frames
		manager->SetResidencyBudgets(budgets);
		const std::vector<AssetHandle> handles = {
			manager->AddStored(0x91000, c_FakeAssetType, EncodeStepped(3, c_MB)),
			manager->AddStored(0x91001, c_FakeAssetType, EncodeStepped(3, c_MB)),
			manager->AddStored(0x91002, c_FakeAssetType, EncodeStepped(3, c_MB)) };
		// Without a job system the loads are read and decoded right here; they wait for an update to be finalized.
		for (AssetHandle handle : handles)
			manager->RequestLoad(handle);
		const uint32_t beginsBefore = upload->Begins;

		std::vector<uint64_t> uploadedPerFrame;
		for (uint32_t frame = 0; frame < 10 && !std::all_of(handles.begin(), handles.end(), [&](AssetHandle handle)
			{ return manager->GetAssetState(handle) == AssetState::Ready; }); frame++)
		{
			manager->Update();
			uploadedPerFrame.push_back(manager->GetStats().UploadedBytesLastFrame);
			CHECK_FALSE(upload->IsOpen()); // Every batch was submitted within the update
		}
		// 9 MB at 2 MB per frame: a frame overshoots by its first call's first step at most, and arrivals are finalized in
		// order (an asset that ran out of budget stays first in line).
		CHECK(uploadedPerFrame == std::vector<uint64_t> { 2 * c_MB, 2 * c_MB, 2 * c_MB, 2 * c_MB, 1 * c_MB });
		for (AssetHandle handle : handles)
			CHECK(manager->GetAssetState(handle) == AssetState::Ready);
		CHECK(upload->Begins - beginsBefore == 5);
		CHECK(upload->Submits == upload->Begins);
		CHECK(upload->SubmittedSteps == 9);

		// Every call had the batch open; only the first call of a frame was its first; UploadedBytes held what the frame's
		// earlier calls uploaded, and the budget what was left of the frame's.
		REQUIRE(calls.size() == 6);
		uint64_t frame = 0;
		uint64_t uploadedInFrame = 0;
		for (const FinalizeCall& call : calls)
		{
			CAPTURE(call.Frame);
			CHECK(call.HadUpload);
			const bool newFrame = call.Frame != frame;
			if (newFrame)
			{
				frame = call.Frame;
				uploadedInFrame = 0;
			}
			CHECK(call.FirstOfFrame == newFrame);
			CHECK(call.UploadedBefore == uploadedInFrame);
			CHECK(call.UploadBudget == budgets.UploadBytesPerFrame - uploadedInFrame);
			uploadedInFrame += call.Steps * c_MB;
		}
		CHECK(manager->GetStats().Resident.GpuTextures == 9 * c_MB);

		// Updates tell the context what they uploaded; a context that returned idle staging is counted.
		CHECK(upload->EndFrames.size() == uploadedPerFrame.size());
		CHECK(upload->EndFrames == uploadedPerFrame);
		CHECK(manager->GetStats().StagingReleases == 0);
		upload->ReleaseOnEndFrame = true;
		manager->Update();
		CHECK(manager->GetStats().StagingReleases == 1);
		CHECK(upload->EndFrames.back() == 0);
		CHECK(StateCountsMatch(*manager));
	}

	TEST_CASE("Past the deadline only the frame's first step goes")
	{
		std::vector<FinalizeCall> calls;
		ScopedSteppedLoader loader(calls);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const Ref<FakeUploadContext> upload = CreateRef<FakeUploadContext>();
		manager->SetUploadContext(upload);
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.FinalizeMsPerFrame = 0.0f; // The deadline has always passed
		manager->SetResidencyBudgets(budgets);
		const AssetHandle first = manager->AddStored(0x92000, c_FakeAssetType, EncodeStepped(3, c_MB));
		const AssetHandle second = manager->AddStored(0x92001, c_FakeAssetType, EncodeStepped(1, c_MB));
		manager->RequestLoad(first);
		manager->RequestLoad(second);

		// One step per frame, and no second call in a frame: the frame's first finalization makes progress, nothing else.
		CHECK(UpdateUntilReady(*manager, { first, second }, 10) == 4);
		REQUIRE(calls.size() == 4);
		for (const FinalizeCall& call : calls)
		{
			CHECK(call.FirstOfFrame);
			CHECK(call.Steps == 1);
		}
		CHECK(StateCountsMatch(*manager));
	}

	TEST_CASE("An asset awaiting the GPU lets the arrivals behind it go on and is finalized again in a later update")
	{
		std::vector<FinalizeCall> calls;
		ScopedSteppedLoader loader(calls);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const Ref<FakeUploadContext> upload = CreateRef<FakeUploadContext>();
		manager->SetUploadContext(upload);
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.FinalizeMsPerFrame = 60000.0f;
		manager->SetResidencyBudgets(budgets);
		const AssetHandle awaiting = manager->AddStored(0x94000, c_FakeAssetType, EncodeStepped(0, 2));
		const AssetHandle behind = manager->AddStored(0x94001, c_FakeAssetType, EncodeStepped(1, c_MB));
		manager->RequestLoad(awaiting);
		manager->RequestLoad(behind);

		// The first update: the awaiting asset is asked first, then the one behind it finishes in the same update.
		manager->Update();
		CHECK(manager->GetAssetState(awaiting) == AssetState::Loading);
		CHECK(manager->GetAssetState(behind) == AssetState::Ready);
		// It is asked once per update until it is done, first in line.
		manager->Update();
		CHECK(manager->GetAssetState(awaiting) == AssetState::Loading);
		manager->Update();
		CHECK(manager->GetAssetState(awaiting) == AssetState::Ready);
		std::vector<AssetHandle> order;
		for (const FinalizeCall& call : calls)
			order.push_back(call.Handle);
		CHECK(order == std::vector<AssetHandle> { awaiting, behind, awaiting, awaiting });
		CHECK(manager->GetStats().LoadingAssets == 0);

		// A load that has to finish at once does not get to wait: it finishes in its call.
		const AssetHandle synchronous = manager->AddStored(0x94002, c_FakeAssetType, EncodeStepped(0, 5));
		calls.clear();
		REQUIRE(manager->LoadAssetSync(synchronous));
		CHECK(calls.size() == 1);
		CHECK(StateCountsMatch(*manager));
	}

	TEST_CASE("Loads without a budget finish in one call, each in a batch of its own")
	{
		std::vector<FinalizeCall> calls;
		ScopedSteppedLoader loader(calls);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const Ref<FakeUploadContext> upload = CreateRef<FakeUploadContext>();
		manager->SetUploadContext(upload);
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.UploadBytesPerFrame = 1; // Budgets apply to updates only
		manager->SetResidencyBudgets(budgets);

		// LoadAssetSync, ReplaceLoadedAsset and AddMemoryAsset finalize without a budget.
		const AssetHandle loaded = manager->AddStored(0x93000, c_FakeAssetType, EncodeStepped(5, c_MB));
		const uint32_t begins = upload->Begins;
		REQUIRE(manager->LoadAssetSync(loaded));
		REQUIRE(calls.size() == 1);
		CHECK(calls.back().Steps == 5);
		CHECK(calls.back().UploadBudget == std::numeric_limits<uint64_t>::max());
		CHECK(calls.back().HadUpload);

		const Ref<SteppedAsset> replacement = CreateRef<SteppedAsset>(4, c_MB, calls);
		std::string error;
		REQUIRE_MESSAGE(manager->ReplaceLoadedAsset(loaded, replacement, &error), error);
		CHECK(calls.back().Steps == 4);
		CHECK(calls.back().HadUpload);
		CHECK(manager->GetStats().Resident.GpuTextures == 4 * c_MB);

		AssetMetadata metadata;
		metadata.Name = "Stepped";
		manager->AddMemoryAsset(CreateRef<SteppedAsset>(2, c_MB, calls), metadata);
		CHECK(calls.back().Steps == 2);
		CHECK(calls.back().HadUpload);
		CHECK(upload->Begins - begins == 3);
		CHECK(upload->Submits == upload->Begins);
		CHECK_FALSE(upload->IsOpen());
		CHECK(StateCountsMatch(*manager));
	}
}
