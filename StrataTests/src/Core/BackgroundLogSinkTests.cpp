#include <doctest/doctest.h>

#include "Strata/Core/BackgroundLogSink.h"
#include "Strata/Core/Log.h"

#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <string>
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

	// A logger as Log::Init makes them: the background sink, and a flush on warnings and errors.
	spdlog::logger MakeLogger(const std::shared_ptr<BackgroundLogSink>& sink)
	{
		spdlog::logger logger("Test", sink);
		logger.set_level(spdlog::level::trace);
		logger.flush_on(spdlog::level::warn);
		return logger;
	}

	constexpr std::chrono::seconds c_Timeout(30);

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
		REQUIRE(logged.wait_for(c_Timeout) == std::future_status::ready);

		inner->Open();
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
		// its bound again, and the message after that waits for room.
		inner->Close();
		const size_t count = 2 * BackgroundLogSink::c_MaxQueuedMessages + 1;
		std::future<void> logged = std::async(std::launch::async, [&logger, count]()
		{
			for (size_t index = 0; index < count; index++)
				logger.trace("bulk {}", index);
		});
		CHECK(logged.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout);

		inner->Open();
		REQUIRE(logged.wait_for(c_Timeout) == std::future_status::ready);
		logger.flush();
		const std::vector<std::string> written = inner->GetWritten();
		REQUIRE(written.size() == count);
		CHECK(written.back() == "bulk " + std::to_string(count - 1));
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
