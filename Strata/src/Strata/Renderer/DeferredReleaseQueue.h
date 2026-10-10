#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <deque>
#include <vector>

namespace Strata
{

	// Keeps GPU resources alive until the GPU has finished the work submitted before they were released, without waiting
	// for it. NVRHI keeps what a command list references alive until the list completes, but its Vulkan backend does not
	// reference the textures of clears (clearTextureFloat, clearTextureUInt, clearDepthStencilTexture) and resolves: a
	// render target that a submitted command list only cleared (the entity IDs of a view in which nothing is drawn) would
	// be destroyed, and its memory freed, while the GPU still writes it. That is a validation error, and freed memory
	// in use can lose the device. Owners that drop render targets while their frames may be in flight (SceneRenderer when
	// it is resized) hand them over here instead, through Renderer::ReleaseDeferred.
	//
	// Completion is tracked with event queries on the graphics queue, where everything renders, so releases do not
	// depend on frames advancing (tools and tests render without them). Main thread only.
	class DeferredReleaseQueue
	{
	public:
		explicit DeferredReleaseQueue(nvrhi::IDevice* device);
		// Drops the references it still holds: the owner waits for the GPU first (Renderer::Shutdown does).
		~DeferredReleaseQueue();

		DeferredReleaseQueue(const DeferredReleaseQueue&) = delete;
		DeferredReleaseQueue& operator=(const DeferredReleaseQueue&) = delete;

		// Takes over references to resources and drops them once every command list executed so far has completed. Hand
		// resources over after executing the command lists that use them. Also collects what has completed meanwhile, so
		// the queue stays bounded by the GPU's progress when no frames run.
		void Release(std::vector<nvrhi::ResourceHandle> resources);
		// Drops the references whose work has completed. Never waits for the GPU. Called once per frame
		// (Renderer::BeginFrame).
		void Collect();

		// Resources held until the GPU is done with them.
		size_t GetPendingCount() const { return m_PendingCount; }
	private:
		struct Batch
		{
			std::vector<nvrhi::ResourceHandle> Resources;
			nvrhi::EventQueryHandle Completion; // Set after the last command list executed before the release
		};
	private:
		nvrhi::IDevice* m_Device;
		std::deque<Batch> m_Pending; // Oldest first: a batch never completes before an older one
		std::vector<nvrhi::EventQueryHandle> m_IdleQueries;
		size_t m_PendingCount = 0;
	};

}
