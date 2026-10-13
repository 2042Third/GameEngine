#pragma once

#include "Strata/Core/Base.h"

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>

#include <chrono>
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
	// returns once every message logged before it is written and the inner sinks are flushed, whatever other threads log
	// meanwhile: loggers flush on warnings and errors, so those have reached their destination when the call that logged
	// them returns.
	//
	// Stop (also called by the destructor and, for the logs of the process, when the process exits) writes the messages
	// logged before it and those still queued, then ends the thread; messages logged afterwards are written right away by
	// the thread that logs them. A process that ends without its exit handlers (a crash, a second Ctrl+C) loses the info and
	// trace lines still queued (warnings and errors are written before their call returns): FlushFor writes them where the
	// platform gives the process a moment before it ends it.
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

		// Like flush, but waits at most `timeout`. Returns whether every message logged before the call is written and the
		// inner sinks are flushed. Waits for nothing but the writer, so any thread may call it at any time (a console's
		// control handler before the process is ended).
		bool FlushFor(std::chrono::nanoseconds timeout);
		// Writes what is queued and ends the writer thread (idempotent).
		void Stop();
		bool IsRunning() const;
	private:
		void Run();
		void WriteToSinks(const spdlog::details::log_msg& message);
		void FlushSinks();
	private:
		std::vector<spdlog::sink_ptr> m_Sinks;
		// Guards writing to the inner sinks once the thread has stopped (the writer thread is their only user before). The
		// writer holds it while it writes the messages left when it stops, so that threads writing their own messages from then
		// on come after them; recursive for an inner sink that logs on the writer thread meanwhile. Taken under m_Mutex only
		// by the writer: log and flush take it with m_Mutex released.
		std::recursive_mutex m_WriteMutex;

		mutable std::mutex m_Mutex;
		std::condition_variable m_WriterWake;    // Messages, a flush request or Stop for the writer
		std::condition_variable m_ProgressMade;  // The writer wrote (or flushed), or there is room in the queue
		std::deque<spdlog::details::log_msg_buffer> m_Queue;
		uint64_t m_Queued = 0;          // Messages queued since the start
		uint64_t m_Written = 0;         // Of those, written to the inner sinks
		uint64_t m_FlushRequested = 0;  // A flush is due once this many messages are written
		uint64_t m_Flushed = 0;         // The inner sinks were flushed after this many messages
		uint64_t m_StopTarget = 0;      // Stop ends the thread once this many messages are written
		bool m_Stopping = false;
		bool m_Running = false;         // Messages go to the queue (else the threads that log write them)
		bool m_Finished = false;        // The writer wrote and flushed everything queued and ended
		std::thread::id m_WriterId;     // The writer thread's (guarded by m_Mutex)
		std::mutex m_StopMutex;         // Serializes Stop (joining the thread)
		std::thread m_Thread;
	};

}
