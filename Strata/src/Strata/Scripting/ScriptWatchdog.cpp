#include "stpch.h"
#include "Strata/Scripting/ScriptWatchdog.h"

#include "Strata/Core/Platform.h"

namespace Strata
{

	ScriptWatchdog::ScriptWatchdog(std::chrono::milliseconds timeout)
		: m_Timeout(std::max(timeout, std::chrono::milliseconds(1)))
	{
		m_Calls.reserve(8);
		m_Thread = std::thread([this]() { Run(); });
	}

	ScriptWatchdog::~ScriptWatchdog()
	{
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_Stop = true;
		}
		m_Condition.notify_all();
		m_Thread.join();
	}

	void ScriptWatchdog::BeginCall(const char* className, const char* method, UUID entity)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		ActiveCall& call = m_Calls.emplace_back();
		call.ClassName = className ? className : "";
		call.Method = method ? method : "";
		call.Entity = entity;
		call.Start = std::chrono::steady_clock::now();
		call.Serial = m_NextSerial++;
	}

	void ScriptWatchdog::EndCall()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (!m_Calls.empty())
			m_Calls.pop_back();
	}

	void ScriptWatchdog::Run()
	{
		Platform::SetCurrentThreadName("Script Watchdog");
		const std::chrono::milliseconds interval = std::clamp(m_Timeout / 4, std::chrono::milliseconds(1), std::chrono::milliseconds(250));

		std::unique_lock<std::mutex> lock(m_Mutex);
		while (!m_Stop)
		{
			m_Condition.wait_for(lock, interval);
			if (m_Stop || m_Calls.empty())
				continue;

			// The strings of the innermost call stay valid while it is on the stack, which the lock guarantees here.
			const ActiveCall& call = m_Calls.back();
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - call.Start);
			if (call.Serial == m_ReportedSerial || elapsed < m_Timeout)
				continue;

			m_ReportedSerial = call.Serial;
			std::string target = call.ClassName[0] != '\0' ? fmt::format("{}.{}", call.ClassName, call.Method) : std::string(call.Method);
			if (call.Entity.IsValid())
				target += fmt::format(" on entity {}", call.Entity.ToString());

			lock.unlock();
			Log::GetScriptLogger()->error("Script call {} has been running for {} ms; it may be stuck in an infinite loop. Native script code "
				"cannot be interrupted, so the engine waits until it returns.", target, elapsed.count());
			m_ReportCount++;
			lock.lock();
		}
	}

}
