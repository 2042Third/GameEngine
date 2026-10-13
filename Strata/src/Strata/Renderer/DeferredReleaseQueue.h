#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
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
	//
	// Dropping the last reference destroys the resource, which costs the driver about 0.1 ms for a texture: a frame that
	// dropped every completed resource at once held the editor 2.1-2.3 ms for the 20 thumbnails a cache let go of, and
	// would hold it for tens of milliseconds for hundreds. Frames destroy within a time budget (Collect's), the rest in the
	// frames after.
	class DeferredReleaseQueue
	{
	public:
		explicit DeferredReleaseQueue(nvrhi::IDevice* device);
		// Drops the references it still holds: the owner waits for the GPU first (Renderer::Shutdown does).
		~DeferredReleaseQueue();

		DeferredReleaseQueue(const DeferredReleaseQueue&) = delete;
		DeferredReleaseQueue& operator=(const DeferredReleaseQueue&) = delete;

		// What a frame spends destroying the resources whose work has completed (Renderer::BeginFrame).
		static constexpr std::chrono::microseconds c_FrameDestroyBudget { 250 };

		// Takes over references to resources and drops them once every command list executed so far has completed. Hand
		// resources over after executing the command lists that use them. Also drops as many references as it takes over
		// among those whose work has completed (the oldest), so the queue stays bounded by the GPU's progress when no frames
		// run.
		void Release(std::vector<nvrhi::ResourceHandle> resources);
		// Drops the references whose work has completed, the oldest first, until `budget` has passed (at least one per call,
		// so the queue always drains); without a budget, all of them. Never waits for the GPU. Called once per frame with
		// c_FrameDestroyBudget (Renderer::BeginFrame).
		void Collect(std::optional<std::chrono::nanoseconds> budget = std::nullopt);

		// Resources held: until the GPU is done with them, or done with and waiting to be destroyed.
		size_t GetPendingCount() const { return m_PendingCount + m_Completed.size(); }
		// Of those, the ones whose work has completed.
		size_t GetCompletedCount() const { return m_Completed.size(); }
	private:
		// Moves the resources of the batches whose work has completed to m_Completed.
		void TakeCompleted();
	private:
		struct Batch
		{
			std::vector<nvrhi::ResourceHandle> Resources;
			nvrhi::EventQueryHandle Completion; // Set after the last command list executed before the release
		};
	private:
		nvrhi::IDevice* m_Device;
		std::deque<Batch> m_Pending; // Oldest first: a batch never completes before an older one
		std::deque<nvrhi::ResourceHandle> m_Completed; // Done on the GPU, to be destroyed, oldest first
		std::vector<nvrhi::EventQueryHandle> m_IdleQueries;
		size_t m_PendingCount = 0;
	};

}
