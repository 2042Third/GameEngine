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
		Collect();
		std::erase_if(resources, [](const nvrhi::ResourceHandle& resource) { return !resource; });
		if (resources.empty())
			return;

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

	void DeferredReleaseQueue::Collect()
	{
		// Queries complete in submission order, so the first pending batch decides whether any later one can be done.
		while (!m_Pending.empty() && m_Device->pollEventQuery(m_Pending.front().Completion))
		{
			Batch batch = std::move(m_Pending.front());
			m_Pending.pop_front();
			m_PendingCount -= batch.Resources.size();
			m_IdleQueries.push_back(std::move(batch.Completion));
		}
	}

}
