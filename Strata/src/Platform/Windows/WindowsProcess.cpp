#include "stpch.h"
#include "Strata/Core/Process.h"

#include "Platform/Windows/WindowsUtils.h"

namespace Strata
{

	namespace
	{

		// Quotes one argument following the CommandLineToArgvW / MSVC CRT parsing rules.
		std::wstring QuoteArgument(const std::wstring& argument)
		{
			if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
				return argument;

			std::wstring result = L"\"";
			for (auto it = argument.begin();; ++it)
			{
				size_t backslashCount = 0;
				while (it != argument.end() && *it == L'\\')
				{
					++it;
					++backslashCount;
				}

				if (it == argument.end())
				{
					result.append(backslashCount * 2, L'\\');
					break;
				}

				if (*it == L'"')
				{
					result.append(backslashCount * 2 + 1, L'\\');
					result.push_back(L'"');
				}
				else
				{
					result.append(backslashCount, L'\\');
					result.push_back(*it);
				}
			}
			result.push_back(L'"');
			return result;
		}

		// Owns a kernel handle until it is released or the owner goes out of scope, so every early return of Start
		// closes the pipe ends it created.
		class ScopedHandle
		{
		public:
			ScopedHandle() = default;
			~ScopedHandle() { Reset(); }

			ScopedHandle(const ScopedHandle&) = delete;
			ScopedHandle& operator=(const ScopedHandle&) = delete;

			HANDLE Get() const { return m_Handle; }
			HANDLE* Receive()
			{
				Reset();
				return &m_Handle;
			}
			HANDLE Release() { return std::exchange(m_Handle, nullptr); }
			void Reset(HANDLE handle = nullptr)
			{
				if (m_Handle && m_Handle != INVALID_HANDLE_VALUE)
					CloseHandle(m_Handle);
				m_Handle = handle;
			}
			bool IsValid() const { return m_Handle && m_Handle != INVALID_HANDLE_VALUE; }
		private:
			HANDLE m_Handle = nullptr;
		};

		bool OpenNullDevice(ScopedHandle& handle, DWORD access, SECURITY_ATTRIBUTES* security, std::string& error)
		{
			handle.Reset(CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, security, OPEN_EXISTING, 0, nullptr));
			if (handle.IsValid())
				return true;
			error = "Opening the null device failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
			handle.Release();
			return false;
		}

		// A pipe whose parent end (read or write) is not inherited by the child.
		bool CreateChildPipe(ScopedHandle& readEnd, ScopedHandle& writeEnd, bool parentReads, SECURITY_ATTRIBUTES* security, std::string& error)
		{
			if (!CreatePipe(readEnd.Receive(), writeEnd.Receive(), security, 0))
			{
				error = "CreatePipe failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
				return false;
			}
			SetHandleInformation(parentReads ? readEnd.Get() : writeEnd.Get(), HANDLE_FLAG_INHERIT, 0);
			return true;
		}

	}

	Process::~Process()
	{
		Close();
	}

	bool Process::Start(const ProcessSpecification& specification)
	{
		Close();
		m_LastError.clear();
		m_ExitCode.reset();
		{
			std::scoped_lock<std::mutex> lock(m_OutputMutex);
			m_Output.clear();
			m_ErrorOutput.clear();
		}

		const bool redirectHandles = specification.Output != ProcessOutputMode::Inherit;
		if (specification.PipeInput && !redirectHandles)
		{
			m_LastError = "Piped input needs captured or discarded output";
			return false;
		}

		std::wstring commandLine = QuoteArgument(specification.Executable.wstring());
		for (const std::string& argument : specification.Arguments)
		{
			commandLine += L' ';
			commandLine += QuoteArgument(WindowsUtils::Utf8ToWide(argument));
		}

		SECURITY_ATTRIBUTES security = {};
		security.nLength = sizeof(security);
		security.bInheritHandle = TRUE;

		// The child's ends are closed once it has started (it holds its own copies); the parent's ends are kept.
		ScopedHandle outputRead;
		ScopedHandle outputWrite;
		ScopedHandle errorRead;
		ScopedHandle errorWrite;
		ScopedHandle inputRead;
		ScopedHandle inputWrite;

		const bool captureOutput = specification.Output == ProcessOutputMode::Capture || specification.Output == ProcessOutputMode::CaptureSeparate;
		if (captureOutput && !CreateChildPipe(outputRead, outputWrite, true, &security, m_LastError))
			return false;
		if (specification.Output == ProcessOutputMode::CaptureSeparate && !CreateChildPipe(errorRead, errorWrite, true, &security, m_LastError))
			return false;
		if (specification.Output == ProcessOutputMode::Discard && !OpenNullDevice(outputWrite, GENERIC_WRITE, &security, m_LastError))
			return false;
		if (redirectHandles)
		{
			if (specification.PipeInput)
			{
				if (!CreateChildPipe(inputRead, inputWrite, false, &security, m_LastError))
					return false;
			}
			else if (!OpenNullDevice(inputRead, GENERIC_READ, &security, m_LastError))
			{
				return false;
			}
		}

		STARTUPINFOEXW startupInfo = {};
		startupInfo.StartupInfo.cb = sizeof(startupInfo);
		std::vector<uint8_t> attributeStorage;
		// The attribute list refers to this array until it is deleted, after CreateProcessW.
		std::vector<HANDLE> inheritedHandles;
		DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT;
		// A console program with inherited output must keep a console, otherwise its output is lost.
		if (specification.HideWindow && redirectHandles)
			creationFlags |= CREATE_NO_WINDOW;
		if (specification.Detached)
			creationFlags |= CREATE_NEW_PROCESS_GROUP;

		if (redirectHandles)
		{
			// Inherit only the handles meant for the child, never other inheritable handles of this process.
			inheritedHandles = { inputRead.Get(), outputWrite.Get() };
			if (errorWrite.IsValid())
				inheritedHandles.push_back(errorWrite.Get());

			SIZE_T attributeSize = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
			attributeStorage.resize(attributeSize);
			auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
			if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize))
			{
				m_LastError = "InitializeProcThreadAttributeList failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
				return false;
			}
			if (!UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles.data(), inheritedHandles.size() * sizeof(HANDLE), nullptr, nullptr))
			{
				m_LastError = "UpdateProcThreadAttribute failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
				DeleteProcThreadAttributeList(attributeList);
				return false;
			}

			startupInfo.lpAttributeList = attributeList;
			startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startupInfo.StartupInfo.hStdInput = inputRead.Get();
			startupInfo.StartupInfo.hStdOutput = outputWrite.Get();
			startupInfo.StartupInfo.hStdError = errorWrite.IsValid() ? errorWrite.Get() : outputWrite.Get();
			creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
		}

		const std::wstring workingDirectory = specification.WorkingDirectory.wstring();
		PROCESS_INFORMATION processInfo = {};
		const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, redirectHandles ? TRUE : FALSE,
			creationFlags, nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startupInfo.StartupInfo, &processInfo);
		const DWORD createError = ::GetLastError();

		if (startupInfo.lpAttributeList)
			DeleteProcThreadAttributeList(startupInfo.lpAttributeList);

		if (!created)
		{
			m_LastError = fmt::format("Failed to start '{}': {}", WindowsUtils::WideToUtf8(specification.Executable.wstring()), WindowsUtils::GetErrorMessage(createError));
			return false;
		}

		CloseHandle(processInfo.hThread);
		m_ProcessHandle = processInfo.hProcess;
		m_ProcessID = processInfo.dwProcessId;
		m_OutputRead = outputRead.Release();
		m_ErrorRead = errorRead.Release();
		m_InputWrite = inputWrite.Release();
		StartOutputReaders();
		return true;
	}

	void Process::StartOutputReaders()
	{
		m_StopReading = false;
		auto startReader = [this](void* pipe, std::string& target, std::atomic<bool>& finished, std::thread& thread)
		{
			if (!pipe)
				return;
			finished = false;
			thread = std::thread([this, pipe, &target, &finished]()
			{
				char buffer[4096];
				while (!m_StopReading.load())
				{
					DWORD bytesRead = 0;
					if (!ReadFile(static_cast<HANDLE>(pipe), buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0)
						break; // Broken pipe: every writer has exited, or the read was cancelled

					std::scoped_lock<std::mutex> lock(m_OutputMutex);
					target.append(buffer, bytesRead);
				}
				finished = true;
			});
		};
		startReader(m_OutputRead, m_Output, m_ReaderFinished, m_OutputThread);
		startReader(m_ErrorRead, m_ErrorOutput, m_ErrorReaderFinished, m_ErrorThread);
	}

	void Process::StopOutputReaders(std::chrono::milliseconds gracePeriod)
	{
		const auto deadline = std::chrono::steady_clock::now() + gracePeriod;
		auto stopReader = [this, deadline](std::thread& thread, std::atomic<bool>& finished)
		{
			if (!thread.joinable())
				return;

			while (!finished.load() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));

			if (!finished.load())
			{
				// A grandchild may still hold the pipe open; cancel the blocking read.
				m_StopReading = true;
				CancelSynchronousIo(thread.native_handle());
			}
			thread.join();
		};
		stopReader(m_OutputThread, m_ReaderFinished);
		stopReader(m_ErrorThread, m_ErrorReaderFinished);
	}

	bool Process::IsRunning()
	{
		return m_ProcessHandle && !Wait(std::chrono::milliseconds(0)).has_value();
	}

	std::optional<int> Process::Wait(std::optional<std::chrono::milliseconds> timeout)
	{
		if (m_ExitCode)
			return m_ExitCode;
		if (!m_ProcessHandle)
			return std::nullopt;

		const DWORD waitMilliseconds = timeout ? static_cast<DWORD>(timeout->count()) : INFINITE;
		if (WaitForSingleObject(static_cast<HANDLE>(m_ProcessHandle), waitMilliseconds) != WAIT_OBJECT_0)
			return std::nullopt;

		DWORD exitCode = 0;
		GetExitCodeProcess(static_cast<HANDLE>(m_ProcessHandle), &exitCode);
		m_ExitCode = static_cast<int>(exitCode);
		return m_ExitCode;
	}

	bool Process::Terminate()
	{
		if (!m_ProcessHandle || m_ExitCode)
			return false;

		if (!TerminateProcess(static_cast<HANDLE>(m_ProcessHandle), 1))
			return false;

		Wait(std::chrono::milliseconds(5000));
		return true;
	}

	bool Process::WriteInput(std::string_view data)
	{
		if (!m_InputWrite)
			return false;

		while (!data.empty())
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size(), 64 * 1024));
			DWORD written = 0;
			// Fails with ERROR_NO_DATA / ERROR_BROKEN_PIPE once the child has closed its end.
			if (!WriteFile(static_cast<HANDLE>(m_InputWrite), data.data(), chunk, &written, nullptr))
				return false;
			data.remove_prefix(written);
		}
		return true;
	}

	void Process::CloseInput()
	{
		if (!m_InputWrite)
			return;
		CloseHandle(static_cast<HANDLE>(m_InputWrite));
		m_InputWrite = nullptr;
	}

	std::string Process::TakeOutput()
	{
		std::scoped_lock<std::mutex> lock(m_OutputMutex);
		return std::exchange(m_Output, std::string());
	}

	std::string Process::TakeErrorOutput()
	{
		std::scoped_lock<std::mutex> lock(m_OutputMutex);
		return std::exchange(m_ErrorOutput, std::string());
	}

	void Process::Close()
	{
		CloseInput();
		StopOutputReaders(std::chrono::milliseconds(0));
		for (void** pipe : { &m_OutputRead, &m_ErrorRead })
		{
			if (*pipe)
			{
				CloseHandle(static_cast<HANDLE>(*pipe));
				*pipe = nullptr;
			}
		}
		if (m_ProcessHandle)
		{
			CloseHandle(static_cast<HANDLE>(m_ProcessHandle));
			m_ProcessHandle = nullptr;
		}
		m_ProcessID = 0;
	}

	Process::RunResult Process::Run(ProcessSpecification specification, std::optional<std::chrono::milliseconds> timeout)
	{
		if (specification.Output != ProcessOutputMode::CaptureSeparate)
			specification.Output = ProcessOutputMode::Capture;
		specification.PipeInput = false;

		RunResult result;
		Process process;
		if (!process.Start(specification))
		{
			result.Error = process.GetLastError();
			return result;
		}
		result.Started = true;

		std::optional<int> exitCode = process.Wait(timeout);
		if (!exitCode)
		{
			result.TimedOut = true;
			process.Terminate();
			exitCode = process.Wait(std::chrono::milliseconds(5000));
		}

		process.StopOutputReaders(std::chrono::milliseconds(2000));
		result.ExitCode = exitCode.value_or(-1);
		result.Output = process.TakeOutput();
		result.ErrorOutput = process.TakeErrorOutput();
		return result;
	}

}
