#pragma once

#include "Strata/Core/Base.h"

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Strata
{

	// A spdlog sink that hands messages to a thread of its own, which writes them to its inner sinks (the console and the log
	// file) in the order they were logged. Writing a line can block for milliseconds (a console, a pipe whose reader is
	// busy, a file the system is scanning), which must never hold up the thread that logs, such as the frame's. flush()
	// returns once every message logged before it is written and the inner sinks are flushed: loggers flush on warnings and
	// errors, so those have reached their destination when the call that logged them returns.
	//
	// Stop (also called by the destructor and, for the logs of the process, when the process exits) writes what is queued
	// and ends the thread; messages logged afterwards are written right away by the thread that logs them.
	class BackgroundLogSink final : public spdlog::sinks::sink
	{
	public:
		// Messages waiting to be written beyond this make the thread that logs wait for room (a burst no writer keeps up
		// with must not grow without bound).
		static constexpr size_t c_MaxQueuedMessages = 65536;

		explicit BackgroundLogSink(std::vector<spdlog::sink_ptr> sinks);
		~BackgroundLogSink() override;

		BackgroundLogSink(const BackgroundLogSink&) = delete;
		BackgroundLogSink& operator=(const BackgroundLogSink&) = delete;

		void log(const spdlog::details::log_msg& message) override;
		void flush() override;
		// Applied to every inner sink.
		void set_pattern(const std::string& pattern) override;
		void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override;

		// Writes what is queued and ends the writer thread (idempotent).
		void Stop();
		bool IsRunning() const;
	private:
		void Run();
		void WriteToSinks(const spdlog::details::log_msg& message);
		void FlushSinks();
	private:
		std::vector<spdlog::sink_ptr> m_Sinks;
		// Guards writing to the inner sinks once the thread has stopped (the writer thread is their only user before).
		std::mutex m_WriteMutex;

		mutable std::mutex m_Mutex;
		std::condition_variable m_WriterWake;    // Messages, a flush request or Stop for the writer
		std::condition_variable m_ProgressMade;  // The writer wrote (or flushed), or there is room in the queue
		std::deque<spdlog::details::log_msg_buffer> m_Queue;
		uint64_t m_Queued = 0;          // Messages queued since the start
		uint64_t m_Written = 0;         // Of those, written to the inner sinks
		uint64_t m_FlushRequested = 0;  // A flush is due once this many messages are written
		uint64_t m_Flushed = 0;         // The inner sinks were flushed after this many messages
		bool m_Stopping = false;
		bool m_Running = false;
		std::thread::id m_WriterId;     // The writer thread's (guarded by m_Mutex)
		std::mutex m_StopMutex;         // Serializes Stop (joining the thread)
		std::thread m_Thread;
	};

}
