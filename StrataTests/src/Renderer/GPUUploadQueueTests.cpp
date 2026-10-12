#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/UploadQueueTestUtils.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/StagingPool.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureFormat.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The budgeted upload queue every asset upload goes through (Renderer::GetUploadQueue), driven by asset managers the way
// applications drive them: device frames, each with the manager's update.
namespace
{

	template<typename Work>
	void RunDeviceFrame(GPUContext& gpu, Work&& work)
	{
		REQUIRE(gpu.GetDevice().BeginFrame());
		Renderer::BeginFrame();
		work();
		gpu.GetDevice().EndFrame();
	}

	// Frames without work, until releases deferred for frames in flight have happened.
	void SettleDevice(GPUContext& gpu)
	{
		for (uint32_t frame = 0; frame < gpu.GetDevice().GetMaxFramesInFlight() + 2; frame++)
			RunDeviceFrame(gpu, []() {});
		gpu.GetDevice().WaitForIdle();
		gpu.GetNvrhiDevice()->runGarbageCollection();
	}

	glm::u8vec4 GetPatternTexel(uint32_t x, uint32_t y, uint32_t level)
	{
		return glm::u8vec4(static_cast<uint8_t>(x), static_cast<uint8_t>(y), static_cast<uint8_t>((x >> 8) + (y >> 8) * 16), static_cast<uint8_t>(level * 17));
	}

	// A cooked texture of `size` x `size` RGBA8 texels, every texel telling its column, row and level apart.
	std::vector<uint8_t> CookPatternTexture(uint32_t size)
	{
		std::vector<TextureMip> mips;
		for (uint32_t level = 0; level < TextureUtils::CalculateMipCount(size, size); level++)
		{
			TextureMip mip;
			mip.Width = std::max(1u, size >> level);
			mip.Height = mip.Width;
			mip.Data.resize(static_cast<size_t>(mip.Width) * mip.Height * 4);
			for (uint32_t y = 0; y < mip.Height; y++)
			{
				for (uint32_t x = 0; x < mip.Width; x++)
				{
					const glm::u8vec4 texel = GetPatternTexel(x, y, level);
					uint8_t* destination = mip.Data.data() + (static_cast<size_t>(y) * mip.Width + x) * 4;
					destination[0] = texel.r;
					destination[1] = texel.g;
					destination[2] = texel.b;
					destination[3] = texel.a;
				}
			}
			mips.push_back(std::move(mip));
		}
		TextureSpecification specification;
		specification.Format = TextureFormat::RGBA8;
		specification.DebugName = "UploadQueuePattern";
		const Ref<Texture> texture = Texture::Create(specification, std::move(mips));
		REQUIRE(texture);
		return texture->Serialize();
	}

	// The cooked form of a mesh of `vertices` vertices (separate triangles).
	std::vector<uint8_t> CookMesh(uint32_t vertices)
	{
		std::vector<glm::vec3> positions(vertices);
		std::vector<MeshVertexAttributes> attributes(vertices);
		std::vector<uint32_t> indices(vertices / 3 * 3);
		for (uint32_t vertex = 0; vertex < vertices; vertex++)
			positions[vertex] = glm::vec3(static_cast<float>(vertex % 100), static_cast<float>(vertex / 100), 0.0f);
		for (uint32_t index = 0; index < indices.size(); index++)
			indices[index] = index;
		Submesh submesh;
		submesh.VertexCount = vertices;
		submesh.LODs.push_back(MeshLOD { 0, static_cast<uint32_t>(indices.size()) });
		std::string error;
		const Ref<Mesh> mesh = Mesh::Create(std::move(positions), std::move(attributes), std::move(indices), { submesh }, &error);
		REQUIRE_MESSAGE(mesh, error);
		return mesh->Serialize();
	}

	uint64_t GetChainBytes(uint32_t size)
	{
		uint64_t bytes = 0;
		for (uint32_t level = 0; level < TextureUtils::CalculateMipCount(size, size); level++)
			bytes += static_cast<uint64_t>(std::max(1u, size >> level)) * std::max(1u, size >> level) * 4;
		return bytes;
	}

	bool AllReady(const AssetManagerBase& manager, const std::vector<AssetHandle>& handles)
	{
		return std::all_of(handles.begin(), handles.end(), [&](AssetHandle handle) { return manager.GetAssetState(handle) == AssetState::Ready; });
	}

	// Budgets in which time does not limit the frames, however slow the machine (Debug builds, a busy machine).
	AssetResidencyBudgets MakeByteBudgets(const AssetManagerBase& manager, uint64_t uploadBytesPerFrame, uint64_t stagingBytes)
	{
		AssetResidencyBudgets budgets = manager.GetResidencyBudgets();
		budgets.UploadBytesPerFrame = uploadBytesPerFrame;
		budgets.StagingBytes = stagingBytes;
		budgets.FinalizeMsPerFrame = 1000.0f;
		budgets.UploadMsPerFrame = 1000.0f;
		return budgets;
	}

}

TEST_SUITE("GPU.UploadQueue")
{
	TEST_CASE("Uploads run on the graphics queue unless the renderer was asked for the transfer queue")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// The test context initializes the renderer with the default specification.
		CHECK_FALSE(RendererSpecification().TransferQueueUploads);
		CHECK_FALSE(Renderer::GetUploadQueue().UsesTransferQueue());
		CHECK_FALSE(Renderer::GetUploadQueue().GetStats().TransferQueue);
		{
			const ScopedTransferQueue queueMode(true);
			CHECK(queueMode.IsEnabled() == gpu.GetDevice().HasTransferQueue());
			CHECK(Renderer::GetUploadQueue().UsesTransferQueue() == gpu.GetDevice().HasTransferQueue());
		}
		CHECK_FALSE(Renderer::GetUploadQueue().UsesTransferQueue());
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A batch that records nothing submits nothing")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Every asset manager update opens a batch; a frame without uploads must cost no submission.
		GPUUploadQueue& queue = Renderer::GetUploadQueue();
		const uint64_t batches = queue.GetStats().Batches;
		const GPUUploadTicket ticket = queue.GetTicket();
		queue.BeginBatch();
		CHECK(queue.IsBatchOpen());
		CHECK(queue.GetTicket() == ticket);
		queue.SubmitBatch();
		CHECK_FALSE(queue.IsBatchOpen());
		CHECK(queue.GetStats().Batches == batches);
		CHECK(queue.GetTicket() == ticket);
		CHECK(queue.IsComplete(ticket));

		// Through a manager's updates too, once its built-in assets are up (they upload as it is made).
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const uint64_t managerBatches = queue.GetStats().Batches;
		for (int frame = 0; frame < 3; frame++)
			RunDeviceFrame(gpu, [&]() { manager->Update(); });
		CHECK(queue.GetStats().Batches == managerBatches);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("No frame uploads more than its byte budget, and staging stays within its budget, on either queue")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			REQUIRE(queueMode.IsEnabled() == transfer);
			const uint64_t transferStepsBefore = Renderer::GetUploadQueue().GetStats().TransferSteps;
			Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
			std::vector<AssetHandle> handles;
			const std::vector<uint8_t> texture = CookPatternTexture(1024);
			for (uint64_t index = 0; index < 6; index++)
				handles.push_back(manager->AddStored(0x71000 + index, AssetType::Texture, texture));
			const std::vector<uint8_t> mesh = CookMesh(60000);
			for (uint64_t index = 0; index < 2; index++)
				handles.push_back(manager->AddStored(0x72000 + index, AssetType::Mesh, mesh));

			const AssetResidencyBudgets budgets = MakeByteBudgets(*manager, 3ull << 20, 12ull << 20);
			manager->SetResidencyBudgets(budgets);
			for (AssetHandle handle : handles)
				manager->RequestLoad(handle);

			const GPUUploadQueue& queue = Renderer::GetUploadQueue();
			const StagingPool& pool = Renderer::GetStagingPool();
			uint64_t total = 0;
			uint32_t frames = 0;
			while (frames < 100 && !AllReady(*manager, handles))
			{
				RunDeviceFrame(gpu, [&]()
				{
					manager->Update();
					const uint64_t uploaded = manager->GetStats().UploadedBytesLastFrame;
					CHECK(uploaded <= budgets.UploadBytesPerFrame);
					CHECK(queue.GetStats().FrameBytes == uploaded); // The manager's uploads are the frame's
					CHECK(pool.GetIdleBytes() + pool.GetPendingBytes() <= budgets.StagingBytes);
					total += uploaded;
				});
				frames++;
			}
			REQUIRE(AllReady(*manager, handles));
			const uint64_t meshBytes = manager->PeekLoadedAsset(handles.back())->GetMemoryUsage().GpuBuffers;
			CHECK(total == 6 * GetChainBytes(1024) + 2 * meshBytes);
			CHECK(frames >= total / budgets.UploadBytesPerFrame);
			CHECK(manager->GetStats().UploadedBytesWindowMax <= budgets.UploadBytesPerFrame);
			// The steps ran on the queue asked for.
			const uint64_t transferSteps = Renderer::GetUploadQueue().GetStats().TransferSteps - transferStepsBefore;
			if (transfer)
				CHECK(transferSteps > 0);
			else
				CHECK(transferSteps == 0);

			// What arrived is right: a level of each texture.
			for (size_t index = 0; index < 6; index++)
			{
				const Ref<Texture> loaded = std::static_pointer_cast<Texture>(manager->PeekLoadedAsset(handles[index]));
				REQUIRE(loaded);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(loaded->GetGPUTexture(), image, 3));
				CHECK(GetPixelRGBA8(image, image.Width - 1, image.Height - 1) == GetPatternTexel(image.Width - 1, image.Height - 1, 3));
			}

			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("Staging past the pool's pending limit stays alive until the GPU has run the copies from it")
	{
		// Copies from staging into textures are recorded raw (GraphicsDevice::RecordBufferToTextureCopies): their command lists
		// do not reference the staging, so the pool holds all of it until the GPU has run them, also what it does not pool. A
		// pool whose pending limit the batch's steps exceed, as unbudgeted loads of large textures (LoadAssetSync,
		// AddMemoryAsset) exceed the renderer's.
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		{
			constexpr uint32_t c_Size = 1024;
			constexpr uint32_t c_RowsPerStep = 64;
			constexpr uint64_t c_StepBytes = static_cast<uint64_t>(c_Size) * c_RowsPerStep * 4;
			constexpr uint32_t c_Steps = c_Size / c_RowsPerStep;
			StagingPool pool(gpu.GetNvrhiDevice(), 2 * c_StepBytes);
			GPUUploadQueue queue(gpu.GetDevice(), pool);

			nvrhi::TextureDesc desc;
			desc.width = c_Size;
			desc.height = c_Size;
			desc.format = ToNvrhiFormat(TextureFormat::RGBA8);
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "PendingLimitTexture";
			const nvrhi::TextureHandle texture = gpu.GetNvrhiDevice()->createTexture(desc);
			REQUIRE(texture);
			std::vector<uint8_t> texels(static_cast<size_t>(c_Size) * c_Size * 4);
			for (uint32_t y = 0; y < c_Size; y++)
			{
				for (uint32_t x = 0; x < c_Size; x++)
				{
					const glm::u8vec4 texel = GetPatternTexel(x, y, 0);
					uint8_t* destination = texels.data() + (static_cast<size_t>(y) * c_Size + x) * 4;
					destination[0] = texel.r;
					destination[1] = texel.g;
					destination[2] = texel.b;
					destination[3] = texel.a;
				}
			}

			// One batch, unbudgeted, a staging buffer per step: all but the first two steps' staging goes past the limit.
			const AssetFinalizeContext context;
			queue.BeginBatch();
			for (uint32_t row = 0; row < c_Size; row += c_RowsPerStep)
			{
				const GPUTextureRegion region { 0, c_Size, c_Size, row, c_RowsPerStep, texels.data() + static_cast<size_t>(row) * c_Size * 4 };
				REQUIRE(queue.UploadTexture(context, texture, TextureFormat::RGBA8, std::span<const GPUTextureRegion>(&region, 1), GPUUploadQueueKind::Graphics).Recorded);
			}
			CHECK(pool.GetCreatedCount() == c_Steps);
			CHECK(pool.GetPendingBytes() == c_Steps * c_StepBytes); // All of it held
			queue.SubmitBatch();
			queue.WaitForTicket(queue.GetTicket());

			// Once the GPU has run the copies, the limit's worth is idle and the rest is freed.
			CHECK(pool.ReleaseIdle() == 2 * c_StepBytes);
			CHECK(pool.GetPendingBytes() == 0);
			ReadbackImage image;
			REQUIRE(Renderer::ReadTexture(texture, image, 0));
			for (const glm::uvec2 texel : { glm::uvec2(0, 0), glm::uvec2(5, 3 * c_RowsPerStep + 7), glm::uvec2(c_Size - 1, c_Size / 2), glm::uvec2(700, c_Size - 1) })
				CHECK(GetPixelRGBA8(image, texel.x, texel.y) == GetPatternTexel(texel.x, texel.y, 0));
		}
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Mesh and texture uploads both go through the queue")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle texture = manager->AddStored(0x73000, AssetType::Texture, CookPatternTexture(256));
		const AssetHandle mesh = manager->AddStored(0x73001, AssetType::Mesh, CookMesh(3000));

		const GPUUploadQueueStats before = Renderer::GetUploadQueue().GetStats();
		REQUIRE(manager->LoadAssetSync(texture));
		REQUIRE(manager->LoadAssetSync(mesh));
		const GPUUploadQueueStats after = Renderer::GetUploadQueue().GetStats();
		CHECK(after.TextureSteps > before.TextureSteps);
		CHECK(after.BufferSteps > before.BufferSteps);
		CHECK(after.TextureBytes - before.TextureBytes == GetChainBytes(256));
		CHECK(after.BufferBytes - before.BufferBytes == manager->PeekLoadedAsset(mesh)->GetMemoryUsage().GpuBuffers);
		CHECK(after.Batches > before.Batches);

		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A 90 MB texture spreads over frames and is ready only once its last level is complete on the queue that samples it")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		for (const bool transfer : GetQueueModes(gpu))
		{
			CAPTURE(transfer);
			SettleDevice(gpu);
			const ScopedTransferQueue queueMode(transfer);
			REQUIRE(queueMode.IsEnabled() == transfer);
			Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
			const AssetHandle texture = manager->AddStored(0x74000, AssetType::Texture, CookPatternTexture(4096));
			const uint64_t chainBytes = GetChainBytes(4096);
			REQUIRE(chainBytes > 89 * 1000 * 1000);
			// The default upload and staging budgets.
			const AssetResidencyBudgets defaults = manager->GetResidencyBudgets();
			manager->SetResidencyBudgets(MakeByteBudgets(*manager, defaults.UploadBytesPerFrame, defaults.StagingBytes));
			manager->RequestLoad(texture);

			GPUUploadQueue& queue = Renderer::GetUploadQueue();
			const uint64_t bytesBefore = queue.GetStats().TextureBytes;
			uint32_t frames = 0;
			uint32_t recordedFrame = 0; // The frame whose update recorded the last step
			GPUUploadTicket recordedTicket = 0;
			uint32_t readyFrame = 0;
			while (frames < 200 && manager->GetAssetState(texture) != AssetState::Ready)
			{
				frames++;
				RunDeviceFrame(gpu, [&]()
				{
					manager->Update();
					const uint64_t recorded = queue.GetStats().TextureBytes - bytesBefore;
					if (recorded == chainBytes && recordedFrame == 0)
					{
						recordedFrame = frames;
						recordedTicket = queue.GetTicket();
					}
					if (manager->GetAssetState(texture) == AssetState::Ready)
					{
						readyFrame = frames;
						// Every level was recorded; on the transfer queue its copies have completed by now too.
						CHECK(recorded == chainBytes);
						if (transfer)
							CHECK(queue.IsComplete(recordedTicket));
					}
					else
					{
						CHECK(manager->PeekLoadedAsset(texture) == nullptr);
					}
				});
			}
			REQUIRE(manager->GetAssetState(texture) == AssetState::Ready);
			CHECK(frames >= chainBytes / defaults.UploadBytesPerFrame);
			// On the graphics queue it is published in the update that recorded its last step (the frame's rendering runs after
			// the uploads); on the transfer queue once the copies completed and the graphics queue took the texture over.
			if (transfer)
				CHECK(readyFrame > recordedFrame);
			else
				CHECK(readyFrame == recordedFrame);
			gpu.GetDevice().WaitForIdle();
			CHECK(queue.IsComplete(recordedTicket));

			// Every level arrived where it belongs.
			const Ref<Texture> loaded = std::static_pointer_cast<Texture>(manager->PeekLoadedAsset(texture));
			REQUIRE(loaded);
			for (uint32_t level : { 0u, 1u, 6u, 12u })
			{
				CAPTURE(level);
				ReadbackImage image;
				REQUIRE(Renderer::ReadTexture(loaded->GetGPUTexture(), image, level));
				for (const glm::uvec2 texel : { glm::uvec2(0, 0), glm::uvec2(image.Width - 1, image.Height / 2), glm::uvec2(image.Width / 3, image.Height - 1) })
					CHECK(GetPixelRGBA8(image, texel.x, texel.y) == GetPatternTexel(texel.x, texel.y, level));
			}

			manager.reset();
			SettleDevice(gpu);
			CHECK(gpu.GetNewErrorCount() == 0);
		}
	}

	TEST_CASE("Loads that finish at once stay on the graphics queue, also with the transfer queue on")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		SettleDevice(gpu);
		const ScopedTransferQueue queueMode(true);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle texture = manager->AddStored(0x75000, AssetType::Texture, CookPatternTexture(512));
		const AssetHandle mesh = manager->AddStored(0x75001, AssetType::Mesh, CookMesh(3000));

		// LoadAssetSync finishes without waiting for another queue.
		const uint64_t transferSteps = Renderer::GetUploadQueue().GetStats().TransferSteps;
		REQUIRE(manager->LoadAssetSync(texture));
		REQUIRE(manager->LoadAssetSync(mesh));
		CHECK(Renderer::GetUploadQueue().GetStats().TransferSteps == transferSteps);

		// One that started on the transfer queue in an update finishes there, waiting for its copies, when it is loaded
		// synchronously after all.
		manager->UnloadAsset(texture);
		manager->SetResidencyBudgets(MakeByteBudgets(*manager, 256ull << 10, 32ull << 20));
		manager->RequestLoad(texture);
		RunDeviceFrame(gpu, [&]() { manager->Update(); });
		if (queueMode.IsEnabled())
			CHECK(Renderer::GetUploadQueue().GetStats().TransferSteps > transferSteps);
		CHECK(manager->GetAssetState(texture) == AssetState::Loading);
		const Ref<Texture> loaded = std::static_pointer_cast<Texture>(manager->LoadAssetSync(texture));
		REQUIRE(loaded);
		ReadbackImage image;
		REQUIRE(Renderer::ReadTexture(loaded->GetGPUTexture(), image, 0));
		CHECK(GetPixelRGBA8(image, 511, 300) == GetPatternTexel(511, 300, 0));

		manager.reset();
		SettleDevice(gpu);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Steps are sized to the bytes and the time left, with the staging they create, a frame's first one granule at least")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const GPUUploadQueue& queue = Renderer::GetUploadQueue();
		constexpr uint64_t c_Wanted = 64ull << 20;
		constexpr uint64_t c_Granule = 64ull << 10;

		// Without limits: the largest step.
		uint64_t frameUploaded = 0;
		AssetFinalizeContext context;
		context.UploadedBytes = &frameUploaded;
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == GPUUploadQueue::c_MaxStepBytes);
		CHECK(queue.PlanStep(context, 0, 0.0f, 100000, c_Granule) == 100000); // What is wanted, whatever the granule

		// The bytes left: whole granules.
		context.UploadBudget = (1ull << 20) + 1000;
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == 1ull << 20);
		CHECK(queue.PlanStep(context, 1ull << 20, 0.0f, c_Wanted, c_Granule) == 0); // Less than a granule left
		context.UploadBudget = std::numeric_limits<uint64_t>::max();

		// The time left: where the pool has no idle staging for the step, after the time creating it takes, at the measured
		// speed of writing fresh staging...
		StagingPool& pool = Renderer::GetStagingPool();
		pool.ReleaseIdle();
		context.UploadMsBudget = 0.25f;
		GPUUploadQueueStats stats = queue.GetStats();
		REQUIRE(stats.BytesPerMs > 0.0f);
		REQUIRE(stats.ColdBytesPerMs > 0.0f);
		// Steps keep a tenth of the frame's upload time in reserve (without UploadMsFrameBudget the call's is the frame's).
		const auto plannedFor = [&](double milliseconds, float bytesPerMs)
		{
			const uint64_t bytes = std::min(GPUUploadQueue::c_MaxStepBytes, static_cast<uint64_t>(std::max(0.0, milliseconds) * static_cast<double>(bytesPerMs)));
			return std::max(c_Granule, bytes / c_Granule * c_Granule); // The frame's first step: one granule at least
		};
		constexpr double c_Planned = 0.25 - 0.1 * 0.25;
		const uint64_t warm = plannedFor(c_Planned, stats.BytesPerMs);
		REQUIRE_FALSE(pool.HasIdleBuffer(std::bit_floor(warm)));
		const double creationMs = std::min(static_cast<double>(stats.StagingCreateMs), c_Planned * 0.5); // At most half the time left
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == plannedFor(c_Planned - creationMs, stats.ColdBytesPerMs));

		// ... and with staging of that size idle, at the speed of writing it.
		pool.Release(pool.AcquireBuffer(std::bit_floor(warm)));
		SettleDevice(gpu);
		REQUIRE(pool.HasIdleBuffer(std::bit_floor(warm)));
		stats = queue.GetStats();
		REQUIRE(plannedFor(c_Planned, stats.BytesPerMs) == warm);
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == warm);
		CHECK(queue.PlanStep(context, c_Granule, 0.25f, c_Wanted, c_Granule) == 0); // Spent: nothing for a later step of the call...

		// A later call of a frame keeps the reserve of the frame's whole upload time, not a share of what is left of it: steps
		// planned within a share of what was left still added up to the budget's end. Of a 2 ms frame, 0.425 ms left plan
		// for 0.225 ms, and 0.15 ms left for nothing.
		context.UploadMsFrameBudget = 2.0f;
		context.UploadMsBudget = 0.425f;
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == warm);
		context.UploadMsBudget = 0.15f;
		frameUploaded = 1; // The frame uploaded already: no granule for its first step
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == 0);
		frameUploaded = 0;
		context.UploadMsFrameBudget = std::numeric_limits<float>::infinity();

		// ... but the frame's first step goes, one granule: every frame makes progress.
		context.UploadMsBudget = 0.0f;
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == c_Granule);
		frameUploaded = 1; // A frame that uploaded something already
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == 0);
		frameUploaded = 0;
		context.FirstOfFrame = false; // Not the frame's first call
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == 0);

		// A deadline that has passed leaves no time either.
		context = AssetFinalizeContext {};
		context.UploadedBytes = &frameUploaded;
		context.Deadline = std::chrono::steady_clock::now();
		CHECK(queue.PlanStep(context, 0, 0.0f, c_Wanted, c_Granule) == c_Granule);
		CHECK(queue.PlanStep(context, c_Granule, 0.0f, c_Wanted, c_Granule) == 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
