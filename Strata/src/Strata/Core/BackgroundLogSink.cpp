#include "stpch.h"
#include "Strata/Core/BackgroundLogSink.h"

#include "Strata/Core/Platform.h"

#include <spdlog/formatter.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iterator>

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
		// The writer has stopped (or this is the writer itself, should an inner sink log): written right away, after what the
		// writer had left (it holds the write lock while it writes that).
		std::scoped_lock<std::recursive_mutex> writeLock(m_WriteMutex);
		WriteToSinks(message);
	}

	void BackgroundLogSink::flush()
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		if (!m_Running || std::this_thread::get_id() == m_WriterId)
		{
			lock.unlock();
			std::scoped_lock<std::recursive_mutex> writeLock(m_WriteMutex);
			FlushSinks();
			return;
		}
		// Everything logged before this call, written and flushed; what other threads log meanwhile is not waited for.
		const uint64_t target = m_Queued;
		m_FlushRequested = std::max(m_FlushRequested, target);
		m_WriterWake.notify_one();
		m_ProgressMade.wait(lock, [this, target]() { return m_Flushed >= target || m_Finished; });
	}

	bool BackgroundLogSink::FlushFor(std::chrono::nanoseconds timeout)
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		// Finished: the threads that log write their own messages, and the writer wrote and flushed what it had.
		if (m_Finished)
			return true;
		// The writer cannot wait for its own progress (an inner sink that flushes the log).
		if (std::this_thread::get_id() == m_WriterId)
			return false;
		const uint64_t target = m_Queued;
		m_FlushRequested = std::max(m_FlushRequested, target);
		m_WriterWake.notify_one();
		return m_ProgressMade.wait_for(lock, timeout, [this, target]() { return m_Flushed >= target || m_Finished; });
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
			if (!m_Stopping)
			{
				m_Stopping = true;
				// The writer ends once the messages logged before this call are written, whatever other threads log meanwhile.
				m_StopTarget = m_Queued;
			}
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

			// A flush is done once the messages logged before it are written, also while other threads go on logging: had it
			// to wait for an empty queue, a thread that logs as fast as the sinks write would hold every warning (and whatever
			// lock its thread holds) until it pauses.
			if (m_FlushRequested > m_Flushed && m_Written >= m_FlushRequested)
			{
				const uint64_t written = m_Written;
				lock.unlock();
				FlushSinks();
				lock.lock();
				m_Flushed = written;
				m_ProgressMade.notify_all();
				continue;
			}

			// Stopping, and what was logged before Stop is written: what was queued since is written here, holding the write
			// lock before the threads that log can take it, so that the messages they write themselves from now on come after.
			// The write lock is taken under m_Mutex only here; log and flush take it with m_Mutex released.
			if (m_Stopping && m_Written >= m_StopTarget)
			{
				std::unique_lock<std::recursive_mutex> writeLock(m_WriteMutex);
				std::deque<spdlog::details::log_msg_buffer> rest;
				rest.swap(m_Queue);
				m_Running = false;
				lock.unlock();
				m_ProgressMade.notify_all(); // Threads waiting for room write their own messages now (after these)
				for (const spdlog::details::log_msg_buffer& message : rest)
					WriteToSinks(message);
				FlushSinks();
				writeLock.unlock();

				lock.lock();
				m_Written += rest.size();
				m_Flushed = m_Written;
				m_Finished = true;
				lock.unlock();
				m_ProgressMade.notify_all();
				return;
			}

			if (!m_Queue.empty())
			{
				// What is queued, written outside the lock: the threads that log go on queueing meanwhile. With a flush due, only
				// up to its messages, so that the thread waiting for it does not wait for messages logged after it as well.
				size_t count = m_Queue.size();
				if (m_FlushRequested > m_Written)
					count = std::min(count, static_cast<size_t>(m_FlushRequested - m_Written));
				std::deque<spdlog::details::log_msg_buffer> batch;
				if (count == m_Queue.size())
				{
					batch.swap(m_Queue);
				}
				else
				{
					const auto end = m_Queue.begin() + static_cast<std::ptrdiff_t>(count);
					batch.insert(batch.end(), std::make_move_iterator(m_Queue.begin()), std::make_move_iterator(end));
					m_Queue.erase(m_Queue.begin(), end);
				}
				lock.unlock();
				m_ProgressMade.notify_all(); // Room in the queue
				for (const spdlog::details::log_msg_buffer& message : batch)
					WriteToSinks(message);
				lock.lock();
				m_Written += batch.size();
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
