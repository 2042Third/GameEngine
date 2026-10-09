#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace Strata
{

	// Reports calls into script code that run longer than a timeout, typically infinite loops. Native code cannot be
	// interrupted safely, so the watchdog only logs an error from its own thread; the call keeps running and the main
	// thread stays blocked until it returns.
	class ScriptWatchdog
	{
	public:
		explicit ScriptWatchdog(std::chrono::milliseconds timeout);
		~ScriptWatchdog();

		ScriptWatchdog(const ScriptWatchdog&) = delete;
		ScriptWatchdog& operator=(const ScriptWatchdog&) = delete;

		// Main thread: brackets one call into script code (calls nest). The strings must stay valid until EndCall.
		void BeginCall(const char* className, const char* method, UUID entity);
		void EndCall();

		std::chrono::milliseconds GetTimeout() const { return m_Timeout; }
		uint64_t GetReportCount() const { return m_ReportCount.load(); }
	private:
		void Run();
	private:
		struct ActiveCall
		{
			const char* ClassName = "";
			const char* Method = "";
			UUID Entity = UUID::Null();
			std::chrono::steady_clock::time_point Start;
			uint64_t Serial = 0;
		};

		const std::chrono::milliseconds m_Timeout;
		std::mutex m_Mutex;
		std::condition_variable m_Condition;
		std::vector<ActiveCall> m_Calls; // Innermost call last
		uint64_t m_NextSerial = 1;
		uint64_t m_ReportedSerial = 0;
		bool m_Stop = false;
		std::atomic<uint64_t> m_ReportCount = 0;
		std::thread m_Thread;
	};

}
