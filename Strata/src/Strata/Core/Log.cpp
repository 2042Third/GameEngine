#include "stpch.h"
#include "Strata/Core/Log.h"

#include "Strata/Core/BackgroundLogSink.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <cstdlib>

namespace Strata
{

	const char* LogLevelToString(LogLevel level)
	{
		switch (level)
		{
			case LogLevel::Trace:    return "Trace";
			case LogLevel::Info:     return "Info";
			case LogLevel::Warn:     return "Warn";
			case LogLevel::Error:    return "Error";
			case LogLevel::Critical: return "Critical";
		}
		return "Unknown";
	}

	static spdlog::level::level_enum ToSpdlogLevel(LogLevel level)
	{
		switch (level)
		{
			case LogLevel::Trace:    return spdlog::level::trace;
			case LogLevel::Info:     return spdlog::level::info;
			case LogLevel::Warn:     return spdlog::level::warn;
			case LogLevel::Error:    return spdlog::level::err;
			case LogLevel::Critical: return spdlog::level::critical;
		}
		return spdlog::level::info;
	}

	static LogLevel FromSpdlogLevel(spdlog::level::level_enum level)
	{
		switch (level)
		{
			case spdlog::level::trace:
			case spdlog::level::debug:    return LogLevel::Trace;
			case spdlog::level::info:     return LogLevel::Info;
			case spdlog::level::warn:     return LogLevel::Warn;
			case spdlog::level::err:      return LogLevel::Error;
			case spdlog::level::critical: return LogLevel::Critical;
			default:                      return LogLevel::Info;
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// LogBuffer
	////////////////////////////////////////////////////////////////////////////////

	LogBuffer::LogBuffer(size_t capacity)
		: m_Capacity(capacity > 0 ? capacity : 1)
	{
	}

	void LogBuffer::Push(LogLevel level, std::string_view logger, std::string_view message, double timestamp)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (m_Entries.size() >= m_Capacity)
			m_Entries.pop_front();

		LogEntry& entry = m_Entries.emplace_back();
		entry.Sequence = m_NextSequence++;
		entry.Level = level;
		entry.Logger = logger;
		entry.Message = message;
		entry.Timestamp = timestamp;
	}

	std::vector<LogEntry> LogBuffer::GetEntries(uint64_t afterSequence, size_t maxCount) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		std::vector<LogEntry> result;
		for (const LogEntry& entry : m_Entries)
		{
			if (entry.Sequence <= afterSequence)
				continue;
			if (result.size() >= maxCount)
				break;
			result.push_back(entry);
		}
		return result;
	}

	uint64_t LogBuffer::GetLatestSequence() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_NextSequence - 1;
	}

	void LogBuffer::Clear()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_Entries.clear();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Log
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// spdlog sink that mirrors every message into the shared LogBuffer.
		class LogBufferSink final : public spdlog::sinks::base_sink<std::mutex>
		{
		public:
			LogBufferSink(LogBuffer& buffer, std::chrono::system_clock::time_point startTime)
				: m_Buffer(buffer), m_StartTime(startTime)
			{
			}
		protected:
			void sink_it_(const spdlog::details::log_msg& message) override
			{
				const double timestamp = std::chrono::duration<double>(message.time - m_StartTime).count();
				m_Buffer.Push(FromSpdlogLevel(message.level),
					std::string_view(message.logger_name.data(), message.logger_name.size()),
					std::string_view(message.payload.data(), message.payload.size()),
					timestamp);
			}

			void flush_() override {}
		private:
			LogBuffer& m_Buffer;
			std::chrono::system_clock::time_point m_StartTime;
		};

		struct LogData
		{
			std::recursive_mutex Mutex;
			std::atomic<bool> Initialized = false;
			Ref<spdlog::logger> CoreLogger;
			Ref<spdlog::logger> ClientLogger;
			Ref<spdlog::logger> ScriptLogger;
			Scope<LogBuffer> Buffer;
			std::filesystem::path LogFile; // The file Init opened
			// Writes the console and the log file on a thread of its own (null without either). Changed under Mutex and
			// WriterMutex, read under either: Log::FlushFor must not wait for Mutex, which Init and Shutdown hold for long.
			Ref<BackgroundLogSink> Writer;
			std::mutex WriterMutex;
			bool ExitHandlerRegistered = false;
		};

		LogData& GetLogData()
		{
			// Intentionally leaked so logging stays valid during static destruction.
			static LogData* s_Data = new LogData();
			return *s_Data;
		}

		// At exit, what is queued for the console and the log file is written before the process ends; messages logged later
		// (static destructors) are written by the threads that log them.
		void StopWriterAtExit()
		{
			LogData& data = GetLogData();
			Ref<BackgroundLogSink> writer;
			{
				std::scoped_lock<std::recursive_mutex> lock(data.Mutex);
				writer = data.Writer;
			}
			if (writer)
				writer->Stop();
		}

	}

	void Log::Init(const LogSpecification& specification)
	{
		LogData& data = GetLogData();
		std::scoped_lock<std::recursive_mutex> lock(data.Mutex);
		if (data.Initialized)
			Shutdown();

		data.Buffer = CreateScope<LogBuffer>(specification.BufferCapacity);
		data.LogFile.clear();

		// The shared buffer is filled at once (the editor's console and automation read it); the console and the log file are
		// written by the background writer, so that a log call never waits for them (BackgroundLogSink).
		std::vector<spdlog::sink_ptr> sinks;
		sinks.push_back(std::make_shared<LogBufferSink>(*data.Buffer, std::chrono::system_clock::now()));
		std::vector<spdlog::sink_ptr> writtenSinks;

		if (specification.ConsoleOutput)
		{
			auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
			consoleSink->set_pattern("%^[%T] %n: %v%$");
			writtenSinks.push_back(consoleSink);
		}

		if (!specification.LogFile.empty())
		{
			try
			{
				std::error_code error;
				if (specification.LogFile.has_parent_path())
					std::filesystem::create_directories(specification.LogFile.parent_path(), error);

				auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(specification.LogFile.string(), true);
				fileSink->set_pattern("[%Y-%m-%d %T.%e] [%l] %n: %v");
				writtenSinks.push_back(fileSink);
				data.LogFile = specification.LogFile;
			}
			catch (const spdlog::spdlog_ex& exception)
			{
				std::fprintf(stderr, "Strata: failed to open log file '%s': %s\n", specification.LogFile.string().c_str(), exception.what());
			}
		}

		if (!writtenSinks.empty())
		{
			Ref<BackgroundLogSink> writer = CreateRef<BackgroundLogSink>(std::move(writtenSinks));
			sinks.push_back(writer);
			{
				std::scoped_lock<std::mutex> writerLock(data.WriterMutex);
				data.Writer = std::move(writer);
			}
			if (!data.ExitHandlerRegistered)
				data.ExitHandlerRegistered = std::atexit(&StopWriterAtExit) == 0;
		}

		const spdlog::level::level_enum level = ToSpdlogLevel(specification.Level);
		// Warnings and errors flush the sinks, which waits for the writer: they are on the console and in the file when the
		// call that logged them returns (and survive a crash right after).
		auto createLogger = [&](const char* name)
		{
			auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
			logger->set_level(level);
			logger->flush_on(spdlog::level::warn);
			return logger;
		};

		data.CoreLogger = createLogger("Strata");
		data.ClientLogger = createLogger("App");
		data.ScriptLogger = createLogger("Script");
		data.Initialized = true;
	}

	void Log::Shutdown()
	{
		LogData& data = GetLogData();
		std::scoped_lock<std::recursive_mutex> lock(data.Mutex);
		if (!data.Initialized)
			return;

		data.CoreLogger->flush();
		data.ClientLogger->flush();
		data.ScriptLogger->flush();
		data.Initialized = false;
		data.CoreLogger.reset();
		data.ClientLogger.reset();
		data.ScriptLogger.reset();
		// Writes what is left and ends the writer's thread.
		if (data.Writer)
			data.Writer->Stop();
		std::scoped_lock<std::mutex> writerLock(data.WriterMutex);
		data.Writer.reset();
	}

	bool Log::IsInitialized()
	{
		return GetLogData().Initialized.load();
	}

	std::filesystem::path Log::GetLogFile()
	{
		LogData& data = GetLogData();
		std::scoped_lock<std::recursive_mutex> lock(data.Mutex);
		return data.Initialized ? data.LogFile : std::filesystem::path();
	}

	static LogData& GetInitializedLogData()
	{
		LogData& data = GetLogData();
		if (!data.Initialized.load()) [[unlikely]]
		{
			std::scoped_lock<std::recursive_mutex> lock(data.Mutex);
			if (!data.Initialized)
				Log::Init();
		}
		return data;
	}

	Ref<spdlog::logger>& Log::GetCoreLogger()
	{
		return GetInitializedLogData().CoreLogger;
	}

	Ref<spdlog::logger>& Log::GetClientLogger()
	{
		return GetInitializedLogData().ClientLogger;
	}

	Ref<spdlog::logger>& Log::GetScriptLogger()
	{
		return GetInitializedLogData().ScriptLogger;
	}

	LogBuffer& Log::GetBuffer()
	{
		return *GetInitializedLogData().Buffer;
	}

	void Log::SetLevel(LogLevel level)
	{
		LogData& data = GetInitializedLogData();
		const spdlog::level::level_enum spdLevel = ToSpdlogLevel(level);
		data.CoreLogger->set_level(spdLevel);
		data.ClientLogger->set_level(spdLevel);
		data.ScriptLogger->set_level(spdLevel);
	}

	void Log::Flush()
	{
		LogData& data = GetLogData();
		std::scoped_lock<std::recursive_mutex> lock(data.Mutex);
		if (!data.Initialized)
			return;

		data.CoreLogger->flush();
		data.ClientLogger->flush();
		data.ScriptLogger->flush();
	}

	bool Log::FlushFor(std::chrono::milliseconds timeout)
	{
		LogData& data = GetLogData();
		Ref<BackgroundLogSink> writer;
		{
			std::scoped_lock<std::mutex> writerLock(data.WriterMutex);
			writer = data.Writer;
		}
		// Without a writer, the sinks are written by the threads that log, which nothing waits for here.
		return !writer || writer->FlushFor(timeout);
	}

}
