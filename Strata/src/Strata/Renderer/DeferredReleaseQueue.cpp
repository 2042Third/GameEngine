#include "stpch.h"
#include "Strata/Renderer/DeferredReleaseQueue.h"

namespace Strata
{

	DeferredReleaseQueue::DeferredReleaseQueue(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	DeferredReleaseQueue::~DeferredReleaseQueue() = default;

	void DeferredReleaseQueue::Release(std::vector<nvrhi::ResourceHandle> resources)
	{
		std::erase_if(resources, [](const nvrhi::ResourceHandle& resource) { return !resource; });
		if (resources.empty())
			return;

		// As many resources as the caller hands over, the oldest done on the GPU, are destroyed now: the queue stays bounded
		// by the GPU's progress when no frames run, and a release costs what dropping its own resources at once would.
		TakeCompleted();
		for (size_t index = 0; index < resources.size() && !m_Completed.empty(); index++)
			m_Completed.pop_front();

		nvrhi::EventQueryHandle completion;
		if (!m_IdleQueries.empty())
		{
			completion = std::move(m_IdleQueries.back());
			m_IdleQueries.pop_back();
		}
		else
		{
			completion = m_Device->createEventQuery();
		}
		if (!completion)
		{
			// Completion cannot be observed without a query: wait for the GPU instead, then drop the resources.
			ST_CORE_ERROR("DeferredReleaseQueue: failed to create an event query; waiting for the GPU to release {} resources", resources.size());
			m_Device->waitForIdle();
			return;
		}
		// The query completes with the last command list executed so far, after every one that may use the resources.
		m_Device->resetEventQuery(completion);
		m_Device->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
		m_PendingCount += resources.size();
		m_Pending.push_back(Batch { std::move(resources), std::move(completion) });
	}

	void DeferredReleaseQueue::Collect(std::optional<std::chrono::nanoseconds> budget)
	{
		TakeCompleted();

		// Dropping the queue's reference destroys what nothing else holds.
		const auto start = std::chrono::steady_clock::now();
		while (!m_Completed.empty())
		{
			m_Completed.pop_front();
			if (budget && std::chrono::steady_clock::now() - start >= *budget)
				break;
		}
	}

	void DeferredReleaseQueue::TakeCompleted()
	{
		// Queries complete in submission order, so the first pending batch decides whether any later one can be done.
		while (!m_Pending.empty() && m_Device->pollEventQuery(m_Pending.front().Completion))
		{
			Batch batch = std::move(m_Pending.front());
			m_Pending.pop_front();
			m_PendingCount -= batch.Resources.size();
			for (nvrhi::ResourceHandle& resource : batch.Resources)
				m_Completed.push_back(std::move(resource));
			m_IdleQueries.push_back(std::move(batch.Completion));
		}
	}

}
