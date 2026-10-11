#include "stpch.h"
#include "Strata/Renderer/RendererUploadContext.h"

#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/StagingPool.h"

namespace Strata
{

	RendererUploadContext::RendererUploadContext(GPUUploadQueue& queue, StagingPool& staging)
		: m_Queue(queue), m_Staging(staging)
	{
	}

	RendererUploadContext::~RendererUploadContext()
	{
		ST_CORE_ASSERT(m_OpenBatches == 0, "An upload batch was opened and never submitted");
	}

	void RendererUploadContext::Begin()
	{
		if (m_OpenBatches++ == 0)
			m_Queue.BeginBatch(); // Later ones join the open batch
	}

	void RendererUploadContext::Submit()
	{
		ST_CORE_ASSERT(m_OpenBatches > 0, "An upload batch was submitted that was never opened");
		if (m_OpenBatches == 0 || --m_OpenBatches > 0)
			return;
		m_Queue.SubmitBatch();
	}

	bool RendererUploadContext::EndFrame(uint64_t uploadedBytes)
	{
		if (uploadedBytes > 0)
		{
			m_FramesWithoutUploads = 0;
			return false;
		}
		if (++m_FramesWithoutUploads < StagingPool::c_IdleReleaseFrames)
			return false;
		// Uploads stopped a while ago: the staging kept for reuse goes, also when no device frames run (StagingPool::BeginFrame
		// does the same with them).
		m_FramesWithoutUploads = 0;
		return m_Staging.ReleaseIdle() > 0;
	}

	nvrhi::ICommandList* RendererUploadContext::GetCommandList() const
	{
		return m_Queue.GetCommandList();
	}

	RendererUploadContext* RendererUploadContext::From(AssetUploadContext* context)
	{
		return dynamic_cast<RendererUploadContext*>(context);
	}

	RendererAssetGraphics& RendererAssetGraphics::Get()
	{
		static RendererAssetGraphics s_Instance;
		return s_Instance;
	}

	AssetUploadContext* RendererAssetGraphics::GetUploadContext()
	{
		return Renderer::IsInitialized() ? &Renderer::GetUploadContext() : nullptr;
	}

	uint32_t RendererAssetGraphics::GetFramesInFlight() const
	{
		return Renderer::IsInitialized() ? Renderer::GetGraphicsDevice().GetMaxFramesInFlight() : 0;
	}

	uint64_t RendererAssetGraphics::GetMemoryBudget() const
	{
		return Renderer::IsInitialized() ? Renderer::GetGraphicsDevice().GetMemoryBudget().Budget : 0;
	}

}
