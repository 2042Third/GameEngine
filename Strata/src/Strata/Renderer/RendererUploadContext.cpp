#include "stpch.h"
#include "Strata/Renderer/RendererUploadContext.h"

#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/StagingPool.h"

namespace Strata
{

	RendererUploadContext::RendererUploadContext(nvrhi::IDevice* device, StagingPool& staging)
		: m_Device(device), m_Staging(staging)
	{
	}

	RendererUploadContext::~RendererUploadContext()
	{
		ST_CORE_ASSERT(m_OpenBatches == 0, "An upload batch was opened and never submitted");
	}

	void RendererUploadContext::Begin()
	{
		if (m_OpenBatches++ > 0)
			return; // Joins the open batch
		if (!m_CommandList)
			m_CommandList = m_Device->createCommandList();
		m_CommandList->open();
	}

	void RendererUploadContext::Submit()
	{
		ST_CORE_ASSERT(m_OpenBatches > 0, "An upload batch was submitted that was never opened");
		if (m_OpenBatches == 0 || --m_OpenBatches > 0)
			return;
		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);
		// The staging these uploads copy from becomes reusable as soon as the GPU has run them, usually by the next frame's
		// finalization: the GPU runs them ahead of the frame's rendering.
		m_Staging.FenceReleased();
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
		return m_OpenBatches > 0 ? m_CommandList.Get() : nullptr;
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
