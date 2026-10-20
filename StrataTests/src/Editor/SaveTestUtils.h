#pragma once

#include <doctest/doctest.h>

#include "TestHelpers.h"

#include <Strata/Core/JobSystem.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

// Helpers of the tests of background saves (DocumentSaver, scene.save): a job system whose background pool is one thread,
// and a gate that holds that thread, so that a save submitted meanwhile stays in flight across frames until the test opens
// the gate (its job waits in the queue; a thread that waits for it runs it itself). IOGate holds its I/O threads the same
// way (a cook's cell reads).
namespace Strata::Tests
{
	constexpr uint32_t c_GatedIOThreads = 2;


	struct GatedJobSystem
	{
		GatedJobSystem()
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = 2;
			specification.IOThreadCount = c_GatedIOThreads;
			specification.BackgroundThreadCount = 1;
			JobSystem::Init(specification);
		}

		~GatedJobSystem()
		{
			JobSystem::Shutdown();
		}

		GatedJobSystem(const GatedJobSystem&) = delete;
		GatedJobSystem& operator=(const GatedJobSystem&) = delete;
	};

	class BackgroundGate
	{
	public:
		BackgroundGate()
		{
			m_Job = JobSystem::Submit([this]()
			{
				m_Started.store(true);
				while (!m_Open.load())
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}, JobPriority::Background);
			REQUIRE(WaitUntil([this]() { return m_Started.load(); }));
		}

		~BackgroundGate()
		{
			Open();
		}

		BackgroundGate(const BackgroundGate&) = delete;
		BackgroundGate& operator=(const BackgroundGate&) = delete;

		void Open()
		{
			m_Open.store(true);
			m_Job.Wait();
		}
	private:
		std::atomic<bool> m_Started = false;
		std::atomic<bool> m_Open = false;
		JobHandle m_Job;
	};

	class IOGate
	{
	public:
		IOGate()
		{
			for (JobHandle& job : m_Jobs)
			{
				job = JobSystem::SubmitIO([this]()
				{
					m_Started.fetch_add(1);
					while (!m_Open.load())
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
				});
			}
			REQUIRE(WaitUntil([this]() { return m_Started.load() == c_GatedIOThreads; }));
		}

		~IOGate()
		{
			Open();
		}

		IOGate(const IOGate&) = delete;
		IOGate& operator=(const IOGate&) = delete;

		void Open()
		{
			m_Open.store(true);
			for (JobHandle& job : m_Jobs)
				job.Wait();
		}
	private:
		std::atomic<uint32_t> m_Started = 0;
		std::atomic<bool> m_Open = false;
		std::array<JobHandle, c_GatedIOThreads> m_Jobs;
	};

}
