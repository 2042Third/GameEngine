#pragma once

#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/Renderer.h"

#include <vector>

// Helpers of the GPU tests that run uploads on either queue (GPU.UploadQueue, GPU.TextureStreaming).
namespace Strata::Tests
{

	// Uses the transfer queue for new uploads (where the device has one) while it lives.
	class ScopedTransferQueue
	{
	public:
		explicit ScopedTransferQueue(bool enabled)
			: m_Previous(Renderer::GetUploadQueue().UsesTransferQueue())
		{
			m_Enabled = Renderer::GetUploadQueue().SetTransferQueue(enabled);
		}

		~ScopedTransferQueue()
		{
			Renderer::GetUploadQueue().SetTransferQueue(m_Previous);
		}

		ScopedTransferQueue(const ScopedTransferQueue&) = delete;
		ScopedTransferQueue& operator=(const ScopedTransferQueue&) = delete;

		bool IsEnabled() const { return m_Enabled; }
	private:
		bool m_Previous = false;
		bool m_Enabled = false;
	};

	// The queues uploads are tested on: the graphics queue, and the transfer queue where the device has one.
	inline std::vector<bool> GetQueueModes(GPUContext& gpu)
	{
		if (gpu.GetDevice().HasTransferQueue())
			return { false, true };
		MESSAGE("The device has no dedicated transfer queue: uploads are tested on the graphics queue only");
		return { false };
	}

}
