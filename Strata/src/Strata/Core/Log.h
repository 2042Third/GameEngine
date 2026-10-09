#pragma once

#include "Strata/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

// Strata logs through spdlog with its bundled fmt.
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	enum class LogLevel : uint8_t
	{
		Trace = 0,
		Info,
		Warn,
		Error,
		Critical
	};

	const char* LogLevelToString(LogLevel level);

	struct LogEntry
	{
		uint64_t Sequence = 0;
		LogLevel Level = LogLevel::Info;
		std::string Logger;
		std::string Message;
		double Timestamp = 0.0; // Seconds since Log::Init
	};

	// Thread-safe ring buffer of the most recent log entries. Backs the editor console and the
	// automation API's log access. Sequence numbers increase monotonically and are never reused.
	class LogBuffer
	{
	public:
		explicit LogBuffer(size_t capacity);

		void Push(LogLevel level, std::string_view logger, std::string_view message, double timestamp);

		// Returns entries with a sequence number greater than afterSequence, oldest first.
		std::vector<LogEntry> GetEntries(uint64_t afterSequence = 0, size_t maxCount = SIZE_MAX) const;
		uint64_t GetLatestSequence() const;
		size_t GetCapacity() const { return m_Capacity; }
		void Clear();
	private:
		mutable std::mutex m_Mutex;
		std::deque<LogEntry> m_Entries;
		size_t m_Capacity;
		uint64_t m_NextSequence = 1;
	};

	struct LogSpecification
	{
		std::filesystem::path LogFile; // Empty disables file logging
		bool ConsoleOutput = true;
		LogLevel Level = LogLevel::Trace;
		size_t BufferCapacity = 4096;
	};

	class Log
	{
	public:
		// Initializes the core, client ("App") and script loggers. Safe to call more than once;
		// loggers are created lazily with default settings if used before Init.
		static void Init(const LogSpecification& specification = {});
		static void Shutdown();
		static bool IsInitialized();

		static Ref<spdlog::logger>& GetCoreLogger();
		static Ref<spdlog::logger>& GetClientLogger();
		static Ref<spdlog::logger>& GetScriptLogger();
		static LogBuffer& GetBuffer();

		static void SetLevel(LogLevel level);
		static void Flush();
	};

}

// glm formatting support for log messages
template<glm::length_t L, typename T, glm::qualifier Q>
struct fmt::formatter<glm::vec<L, T, Q>> : fmt::formatter<std::string>
{
	auto format(const glm::vec<L, T, Q>& value, fmt::format_context& context) const
	{
		return fmt::formatter<std::string>::format(glm::to_string(value), context);
	}
};

template<glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
struct fmt::formatter<glm::mat<C, R, T, Q>> : fmt::formatter<std::string>
{
	auto format(const glm::mat<C, R, T, Q>& value, fmt::format_context& context) const
	{
		return fmt::formatter<std::string>::format(glm::to_string(value), context);
	}
};

template<typename T, glm::qualifier Q>
struct fmt::formatter<glm::qua<T, Q>> : fmt::formatter<std::string>
{
	auto format(const glm::qua<T, Q>& value, fmt::format_context& context) const
	{
		return fmt::formatter<std::string>::format(glm::to_string(value), context);
	}
};

// Core (engine) log macros
#define ST_CORE_TRACE(...)    ::Strata::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define ST_CORE_INFO(...)     ::Strata::Log::GetCoreLogger()->info(__VA_ARGS__)
#define ST_CORE_WARN(...)     ::Strata::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define ST_CORE_ERROR(...)    ::Strata::Log::GetCoreLogger()->error(__VA_ARGS__)
#define ST_CORE_CRITICAL(...) ::Strata::Log::GetCoreLogger()->critical(__VA_ARGS__)

// Client (editor/game application) log macros
#define ST_TRACE(...)         ::Strata::Log::GetClientLogger()->trace(__VA_ARGS__)
#define ST_INFO(...)          ::Strata::Log::GetClientLogger()->info(__VA_ARGS__)
#define ST_WARN(...)          ::Strata::Log::GetClientLogger()->warn(__VA_ARGS__)
#define ST_ERROR(...)         ::Strata::Log::GetClientLogger()->error(__VA_ARGS__)
#define ST_CRITICAL(...)      ::Strata::Log::GetClientLogger()->critical(__VA_ARGS__)
