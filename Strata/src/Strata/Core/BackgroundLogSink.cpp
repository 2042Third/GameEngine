#include "stpch.h"
#include "Strata/Core/BackgroundLogSink.h"

#include "Strata/Core/Platform.h"

#include <spdlog/formatter.h>

#include <cstdio>
#include <exception>

namespace Strata
{

	BackgroundLogSink::BackgroundLogSink(std::vector<spdlog::sink_ptr> sinks)
		: m_Sinks(std::move(sinks))
	{
		m_Running = true;
		m_Thread = std::thread([this]()
		{
			Platform::SetCurrentThreadName("Strata Log Writer");
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_WriterId = std::this_thread::get_id();
			}
			Run();
		});
	}

	BackgroundLogSink::~BackgroundLogSink()
	{
		Stop();
	}

	void BackgroundLogSink::log(const spdlog::details::log_msg& message)
	{
		{
			std::unique_lock<std::mutex> lock(m_Mutex);
			if (m_Running && std::this_thread::get_id() != m_WriterId)
			{
				m_ProgressMade.wait(lock, [this]() { return m_Queue.size() < c_MaxQueuedMessages || !m_Running; });
				if (m_Running)
				{
					m_Queue.emplace_back(message);
					m_Queued++;
					lock.unlock();
					m_WriterWake.notify_one();
					return;
				}
			}
		}
		// The writer has stopped (or this is the writer itself, should an inner sink log): written right away.
		std::scoped_lock<std::mutex> writeLock(m_WriteMutex);
		WriteToSinks(message);
	}

	void BackgroundLogSink::flush()
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (!m_Running || std::this_thread::get_id() == m_WriterId)
		{
			lock.unlock();
			std::scoped_lock<std::mutex> writeLock(m_WriteMutex);
			FlushSinks();
			return;
		}
		// Everything logged before this call, written and flushed.
		const uint64_t target = m_Queued;
		m_FlushRequested = std::max(m_FlushRequested, target);
		m_WriterWake.notify_one();
		m_ProgressMade.wait(lock, [this, target]() { return m_Flushed >= target || !m_Running; });
	}

	void BackgroundLogSink::set_pattern(const std::string& pattern)
	{
		for (const spdlog::sink_ptr& sink : m_Sinks)
			sink->set_pattern(pattern);
	}

	void BackgroundLogSink::set_formatter(std::unique_ptr<spdlog::formatter> formatter)
	{
		for (const spdlog::sink_ptr& sink : m_Sinks)
			sink->set_formatter(formatter->clone());
	}

	void BackgroundLogSink::Stop()
	{
		std::scoped_lock<std::mutex> stopLock(m_StopMutex);
		if (!m_Thread.joinable())
			return;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_Stopping = true;
		}
		m_WriterWake.notify_one();
		m_Thread.join();
	}

	bool BackgroundLogSink::IsRunning() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Running;
	}

	void BackgroundLogSink::Run()
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		while (true)
		{
			m_WriterWake.wait(lock, [this]() { return !m_Queue.empty() || m_FlushRequested > m_Flushed || m_Stopping; });
			if (!m_Queue.empty())
			{
				// Everything queued so far, written outside the lock: the threads that log go on queueing meanwhile.
				std::deque<spdlog::details::log_msg_buffer> batch;
				batch.swap(m_Queue);
				lock.unlock();
				m_ProgressMade.notify_all(); // Room in the queue
				for (const spdlog::details::log_msg_buffer& message : batch)
					WriteToSinks(message);
				lock.lock();
				m_Written += batch.size();
				continue;
			}
			// The queue is empty: every message queued is written, so a flush covers all of them.
			if (m_FlushRequested > m_Flushed || m_Stopping)
			{
				const uint64_t written = m_Written;
				lock.unlock();
				FlushSinks();
				lock.lock();
				m_Flushed = written;
				if (m_Stopping && m_Queue.empty())
				{
					// From here on, the threads that log write themselves (log and flush check m_Running under the lock).
					m_Running = false;
					m_ProgressMade.notify_all();
					return;
				}
				m_ProgressMade.notify_all();
			}
		}
	}

	void BackgroundLogSink::WriteToSinks(const spdlog::details::log_msg& message)
	{
		for (const spdlog::sink_ptr& sink : m_Sinks)
		{
			if (!sink->should_log(message.level))
				continue;
			try
			{
				sink->log(message);
			}
			catch (const std::exception& exception)
			{
				// A console or file that cannot be written (a closed pipe, a full disk) loses the line: logging must not fail
				// the program, and logging the failure would only try the same sink again.
				std::fprintf(stderr, "Strata: a log sink failed to write: %s\n", exception.what());
			}
		}
	}

	void BackgroundLogSink::FlushSinks()
	{
		for (const spdlog::sink_ptr& sink : m_Sinks)
		{
			try
			{
				sink->flush();
			}
			catch (const std::exception& exception)
			{
				std::fprintf(stderr, "Strata: a log sink failed to flush: %s\n", exception.what());
			}
		}
	}

}
