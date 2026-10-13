#include <doctest/doctest.h>

#include "Strata/Core/BackgroundLogSink.h"
#include "Strata/Core/Log.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	// An inner sink standing in for a console or a file: it records the messages it writes and, while its gate is closed,
	// blocks every write (a console the person scrolls, a pipe whose reader is busy).
	class GatedSink final : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		void Close()
		{
			std::scoped_lock<std::mutex> lock(m_GateMutex);
			m_Open = false;
		}

		void Open()
		{
			{
				std::scoped_lock<std::mutex> lock(m_GateMutex);
				m_Open = true;
			}
			m_Gate.notify_all();
		}

		std::vector<std::string> GetWritten() const
		{
			std::scoped_lock<std::mutex> lock(m_GateMutex);
			return m_Written;
		}

		size_t GetFlushCount() const
		{
			std::scoped_lock<std::mutex> lock(m_GateMutex);
			return m_Flushes;
		}
	protected:
		void sink_it_(const spdlog::details::log_msg& message) override
		{
			std::unique_lock<std::mutex> lock(m_GateMutex);
			m_Gate.wait(lock, [this]() { return m_Open; });
			m_Written.emplace_back(message.payload.data(), message.payload.size());
		}

		void flush_() override
		{
			std::scoped_lock<std::mutex> lock(m_GateMutex);
			m_Flushes++;
		}
	private:
		mutable std::mutex m_GateMutex;
		std::condition_variable m_Gate;
		bool m_Open = true;
		std::vector<std::string> m_Written;
		size_t m_Flushes = 0;
	};

	// An inner sink whose every write takes a while (a slow console), recording what it wrote.
	class SlowSink final : public spdlog::sinks::base_sink<std::mutex>
	{
	public:
		explicit SlowSink(std::chrono::microseconds writeTime)
			: m_WriteTime(writeTime)
		{
		}

		std::vector<std::string> GetWritten() const
		{
			std::scoped_lock<std::mutex> lock(m_RecordMutex);
			return m_Written;
		}

		bool HasWritten(const std::string& text) const
		{
			std::scoped_lock<std::mutex> lock(m_RecordMutex);
			return std::find(m_Written.begin(), m_Written.end(), text) != m_Written.end();
		}
	protected:
		void sink_it_(const spdlog::details::log_msg& message) override
		{
			// Busy, as a console write is (sleeping would take at least a scheduler tick).
			const auto end = std::chrono::steady_clock::now() + m_WriteTime;
			while (std::chrono::steady_clock::now() < end)
				std::this_thread::yield();
			std::scoped_lock<std::mutex> lock(m_RecordMutex);
			m_Written.emplace_back(message.payload.data(), message.payload.size());
		}

		void flush_() override {}
	private:
		const std::chrono::microseconds m_WriteTime;
		mutable std::mutex m_RecordMutex;
		std::vector<std::string> m_Written;
	};

	// A logger as Log::Init makes them: the background sink, and a flush on warnings and errors.
	spdlog::logger MakeLogger(const std::shared_ptr<BackgroundLogSink>& sink)
	{
		spdlog::logger logger("Test", sink);
		logger.set_level(spdlog::level::trace);
		logger.flush_on(spdlog::level::warn);
		return logger;
	}

	constexpr std::chrono::seconds c_Timeout(30);

	// A thread that logs "busy <n>" a little faster than the slow sink writes, so the writer never finds the queue empty,
	// until it is stopped (when it goes out of scope at the latest) or for `limit`.
	class BusyLogger
	{
	public:
		BusyLogger(spdlog::logger& logger, std::chrono::microseconds interval, std::chrono::seconds limit)
		{
			m_Thread = std::thread([this, &logger, interval, limit]()
			{
				const auto start = std::chrono::steady_clock::now();
				for (uint64_t index = 0; !m_StopRequested && std::chrono::steady_clock::now() < start + limit; index++)
				{
					const auto due = start + interval * static_cast<int64_t>(index);
					while (std::chrono::steady_clock::now() < due)
						std::this_thread::yield();
					logger.info("busy {}", index);
					m_Logged++;
				}
				m_Logging = false;
			});
		}

		~BusyLogger()
		{
			Stop();
		}

		BusyLogger(const BusyLogger&) = delete;
		BusyLogger& operator=(const BusyLogger&) = delete;

		void Stop()
		{
			m_StopRequested = true;
			if (m_Thread.joinable())
				m_Thread.join();
		}

		bool IsLogging() const { return m_Logging; }
		uint64_t GetLogged() const { return m_Logged; }
	private:
		std::atomic<bool> m_StopRequested = false;
		std::atomic<bool> m_Logging = true;
		std::atomic<uint64_t> m_Logged = 0;
		std::thread m_Thread;
	};

}

TEST_SUITE("Core.Log")
{
	TEST_CASE("Log calls return while the console or file they go to is blocked, and the messages follow in order")
	{
		const auto inner = std::make_shared<GatedSink>();
		const auto sink = std::make_shared<BackgroundLogSink>(std::vector<spdlog::sink_ptr> { inner });
		spdlog::logger logger = MakeLogger(sink);

		// With the inner sink blocked, the calls still return: they only queue (were they written by the calling thread,
		// the first would wait for the gate).
		inner->Close();
		std::future<void> logged = std::async(std::launch::async, [&logger]()
		{
			for (int index = 0; index < 5; index++)
				logger.info("message {}", index);
		});
		const bool returned = logged.wait_for(c_Timeout) == std::future_status::ready;
		// Opened before anything can end the test: the future's destructor waits for calls that wait for the gate.
		inner->Open();
		REQUIRE(returned);

		logger.flush();
		const std::vector<std::string> written = inner->GetWritten();
		REQUIRE(written.size() == 5);
		for (size_t index = 0; index < written.size(); index++)
			CHECK(written[index] == "message " + std::to_string(index));
		CHECK(inner->GetFlushCount() >= 1);
	}

	TEST_CASE("Warnings and errors are written when the call that logs them returns")
	{
		const auto inner = std::make_shared<GatedSink>();
		const auto sink = std::make_shared<BackgroundLogSink>(std::vector<spdlog::sink_ptr> { inner });
		spdlog::logger logger = MakeLogger(sink);

		logger.info("before");
		logger.warn("warning");
		// The flush the warning triggers waited for the writer: the warning and everything before it are written.
		std::vector<std::string> written = inner->GetWritten();
		REQUIRE(written.size() == 2);
		CHECK(written[0] == "before");
		CHECK(written[1] == "warning");

		logger.error("error");
		written = inner->GetWritten();
		REQUIRE(written.size() == 3);
		CHECK(written[2] == "error");
	}

	TEST_CASE("A warning and Stop wait for what was logged before them, not for what another thread keeps logging")
	{
		// The writer takes 100 microseconds a line and another thread logs a line every 75: the queue is never empty. A
		// warning (which flushes) and Stop returned only once the writer found the queue empty, so they waited for as long
		// as the other thread logged, holding whatever lock the warning's thread held.
		const auto inner = std::make_shared<SlowSink>(std::chrono::microseconds(100));
		const auto sink = std::make_shared<BackgroundLogSink>(std::vector<spdlog::sink_ptr> { inner });
		spdlog::logger logger = MakeLogger(sink);
		BusyLogger busy(logger, std::chrono::microseconds(75), std::chrono::seconds(20));
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		REQUIRE(busy.IsLogging());

		struct Outcome
		{
			bool Written = false;      // The warning was written when the call returned
			bool OtherLogging = false; // The other thread was still logging then
		};
		std::future<Outcome> warned = std::async(std::launch::async, [&logger, &inner, &busy]()
		{
			logger.warn("warning");
			return Outcome { inner->HasWritten("warning"), busy.IsLogging() };
		});
		REQUIRE(warned.wait_for(c_Timeout) == std::future_status::ready);
		const Outcome warning = warned.get();
		CHECK(warning.Written);
		CHECK(warning.OtherLogging);

		std::future<bool> stopped = std::async(std::launch::async, [&sink, &busy]()
		{
			sink->Stop();
			return busy.IsLogging();
		});
		REQUIRE(stopped.wait_for(c_Timeout) == std::future_status::ready);
		CHECK(stopped.get());
		CHECK_FALSE(sink->IsRunning());

		// Nothing lost and nothing out of order: the queue's messages, then those the thread wrote itself after Stop.
		busy.Stop();
		const std::vector<std::string> written = inner->GetWritten();
		uint64_t next = 0;
		size_t warnings = 0;
		size_t unexpected = 0;
		for (const std::string& message : written)
		{
			if (message == "busy " + std::to_string(next))
				next++;
			else if (message == "warning")
				warnings++;
			else
				unexpected++;
		}
		CHECK(next == busy.GetLogged());
		CHECK(warnings == 1);
		CHECK(unexpected == 0);
	}

	TEST_CASE("Stopping the writer writes what is queued, and later messages are written by the thread that logs")
	{
		const auto inner = std::make_shared<GatedSink>();
		const auto sink = std::make_shared<BackgroundLogSink>(std::vector<spdlog::sink_ptr> { inner });
		spdlog::logger logger = MakeLogger(sink);
		CHECK(sink->IsRunning());

		for (int index = 0; index < 1000; index++)
			logger.trace("queued {}", index);
		sink->Stop();
		CHECK_FALSE(sink->IsRunning());
		std::vector<std::string> written = inner->GetWritten();
		REQUIRE(written.size() == 1000);
		CHECK(written.front() == "queued 0");
		CHECK(written.back() == "queued 999");

		// Stopped (as at process exit): written at once, without a flush.
		logger.info("after");
		written = inner->GetWritten();
		REQUIRE(written.size() == 1001);
		CHECK(written.back() == "after");
		sink->Stop(); // Idempotent
	}

	TEST_CASE("A full queue makes the thread that logs wait until the writer has room")
	{
		const auto inner = std::make_shared<GatedSink>();
		const auto sink = std::make_shared<BackgroundLogSink>(std::vector<spdlog::sink_ptr> { inner });
		spdlog::logger logger = MakeLogger(sink);

		// The writer takes what is queued when it wakes (at most the bound) and blocks writing it; the queue then fills up to
		// its bound again, and the call after that waits for room: at most twice the bound of calls return.
		inner->Close();
		constexpr size_t c_Bound = BackgroundLogSink::c_MaxQueuedMessages;
		const size_t count = 2 * c_Bound + 1;
		std::atomic<size_t> returned = 0;
		std::future<void> logged = std::async(std::launch::async, [&logger, &returned, count]()
		{
			for (size_t index = 0; index < count; index++)
			{
				logger.trace("bulk {}", index);
				returned++;
			}
		});
		// Until the calls stop returning (for a quarter of a second, past the bound) or all of them returned.
		const auto deadline = std::chrono::steady_clock::now() + c_Timeout;
		size_t settled = returned;
		auto settledSince = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			const size_t now = returned;
			if (now != settled)
			{
				settled = now;
				settledSince = std::chrono::steady_clock::now();
			}
			if (settled == count || (settled > c_Bound && std::chrono::steady_clock::now() - settledSince > std::chrono::milliseconds(250)))
				break;
		}
		// Opened before anything can end the test: the future's destructor waits for the calls.
		inner->Open();
		CHECK(settled > c_Bound);
		CHECK(settled <= 2 * c_Bound);

		REQUIRE(logged.wait_for(c_Timeout) == std::future_status::ready);
		logger.flush();
		const std::vector<std::string> written = inner->GetWritten();
		REQUIRE(written.size() == count);
		CHECK(written.back() == "bulk " + std::to_string(count - 1));
	}

	TEST_CASE("A bounded flush waits at most its time for a blocked console, and writes everything once it can")
	{
		// What a console's control handler does before Windows ends the process (a second Ctrl+C): the writer gets a moment
		// to write the last lines, and the handler is not held beyond it by a console that does not take them.
		const auto inner = std::make_shared<GatedSink>();
		const auto sink = std::make_shared<BackgroundLogSink>(std::vector<spdlog::sink_ptr> { inner });
		spdlog::logger logger = MakeLogger(sink);

		inner->Close();
		logger.info("last line");
		const auto start = std::chrono::steady_clock::now();
		const bool flushed = sink->FlushFor(std::chrono::milliseconds(50));
		const auto waited = std::chrono::steady_clock::now() - start;
		inner->Open();
		CHECK_FALSE(flushed);
		CHECK(waited >= std::chrono::milliseconds(40));
		CHECK(waited < c_Timeout);

		CHECK(sink->FlushFor(c_Timeout));
		CHECK(inner->GetWritten() == std::vector<std::string> { "last line" });
		CHECK(inner->GetFlushCount() >= 1);

		// Stopped: nothing is left for the writer.
		sink->Stop();
		CHECK(sink->FlushFor(std::chrono::milliseconds(0)));
		// The process's logs: the test runner's writes its console.
		CHECK(Log::FlushFor(c_Timeout));
	}

	TEST_CASE("The engine's loggers write the console and the log file through the background writer")
	{
		// Log::Init (the test runner logs to the console): the console and the file sinks sit behind a BackgroundLogSink, so
		// that a log call on the frame's thread never waits for them.
		for (const Ref<spdlog::logger>& logger : { Log::GetCoreLogger(), Log::GetClientLogger(), Log::GetScriptLogger() })
		{
			const std::vector<spdlog::sink_ptr>& sinks = logger->sinks();
			const bool background = std::any_of(sinks.begin(), sinks.end(), [](const spdlog::sink_ptr& sink)
			{
				return std::dynamic_pointer_cast<BackgroundLogSink>(sink) != nullptr;
			});
			CHECK(background);
		}
	}
}
