#pragma once

#include "Strata/Asset/AssetUploadContext.h"
#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Strata
{

	class GPUUploadQueue;
	class StagingPool;

	// The renderer's AssetUploadContext: the asset managers' batches of uploads (AssetFinalizeContext::Upload) are batches of
	// the renderer's budgeted upload queue (GPUUploadQueue), which the renderer's assets record into. EndFrame returns idle
	// staging once uploads stopped for StagingPool::c_IdleReleaseFrames manager updates. The renderer owns one
	// (Renderer::GetUploadContext), handed to the asset managers by the IAssetGraphics engine service. Main thread only.
	class RendererUploadContext final : public AssetUploadContext
	{
	public:
		RendererUploadContext(GPUUploadQueue& queue, StagingPool& staging);
		~RendererUploadContext() override;

		RendererUploadContext(const RendererUploadContext&) = delete;
		RendererUploadContext& operator=(const RendererUploadContext&) = delete;

		void Begin() override;
		void Submit() override;
		bool EndFrame(uint64_t uploadedBytes) override;

		// The queue the batches record into.
		GPUUploadQueue& GetQueue() const { return m_Queue; }
		// The command list of the open batch; null while no batch is open.
		nvrhi::ICommandList* GetCommandList() const;
		// The renderer's context when `context` is one, else null (no context, or another implementation such as a test's):
		// what the renderer's assets record their uploads into.
		static RendererUploadContext* From(AssetUploadContext* context);
	private:
		GPUUploadQueue& m_Queue;
		StagingPool& m_Staging;
		uint32_t m_OpenBatches = 0;       // Begin calls without their Submit (nested batches join the outermost)
		uint32_t m_FramesWithoutUploads = 0;
	};

	// The renderer module's IAssetGraphics engine service (registered by RegisterRendererModule): the running renderer's
	// upload context, frames in flight and memory budget, or none of them while no renderer runs.
	class RendererAssetGraphics final : public IAssetGraphics
	{
	public:
		static RendererAssetGraphics& Get();

		AssetUploadContext* GetUploadContext() override;
		uint32_t GetFramesInFlight() const override;
		uint64_t GetMemoryBudget() const override;
	};

}
