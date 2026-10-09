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

		HANDLE OpenNullDevice(DWORD access, SECURITY_ATTRIBUTES* security)
		{
			return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, security, OPEN_EXISTING, 0, nullptr);
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

		HANDLE outputRead = nullptr;
		HANDLE outputWrite = nullptr;
		HANDLE inputRead = nullptr;
		auto closeHandles = [&]()
		{
			if (outputWrite)
				CloseHandle(outputWrite);
			if (inputRead)
				CloseHandle(inputRead);
			outputWrite = nullptr;
			inputRead = nullptr;
		};

		const bool redirectHandles = specification.Output != ProcessOutputMode::Inherit;
		if (specification.Output == ProcessOutputMode::Capture)
		{
			if (!CreatePipe(&outputRead, &outputWrite, &security, 0))
			{
				m_LastError = "CreatePipe failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
				return false;
			}
			SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0);
		}
		else if (specification.Output == ProcessOutputMode::Discard)
		{
			outputWrite = OpenNullDevice(GENERIC_WRITE, &security);
		}

		if (redirectHandles)
			inputRead = OpenNullDevice(GENERIC_READ, &security);

		STARTUPINFOEXW startupInfo = {};
		startupInfo.StartupInfo.cb = sizeof(startupInfo);
		std::vector<uint8_t> attributeStorage;
		DWORD creationFlags = CREATE_UNICODE_ENVIRONMENT;
		// A console program with inherited output must keep a console, otherwise its output is lost.
		if (specification.HideWindow && redirectHandles)
			creationFlags |= CREATE_NO_WINDOW;
		if (specification.Detached)
			creationFlags |= CREATE_NEW_PROCESS_GROUP;

		if (redirectHandles)
		{
			// Inherit only the two handles meant for the child, never other inheritable handles of this process.
			SIZE_T attributeSize = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
			attributeStorage.resize(attributeSize);
			auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
			if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize))
			{
				m_LastError = "InitializeProcThreadAttributeList failed: " + WindowsUtils::GetErrorMessage(::GetLastError());
				closeHandles();
				if (outputRead)
					CloseHandle(outputRead);
				return false;
			}

			HANDLE inheritedHandles[2] = { inputRead, outputWrite };
			UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr);

			startupInfo.lpAttributeList = attributeList;
			startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			startupInfo.StartupInfo.hStdInput = inputRead;
			startupInfo.StartupInfo.hStdOutput = outputWrite;
			startupInfo.StartupInfo.hStdError = outputWrite;
			creationFlags |= EXTENDED_STARTUPINFO_PRESENT;
		}

		const std::wstring workingDirectory = specification.WorkingDirectory.wstring();
		PROCESS_INFORMATION processInfo = {};
		const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, redirectHandles ? TRUE : FALSE,
			creationFlags, nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startupInfo.StartupInfo, &processInfo);
		const DWORD createError = ::GetLastError();

		if (startupInfo.lpAttributeList)
			DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
		closeHandles();

		if (!created)
		{
			if (outputRead)
				CloseHandle(outputRead);
			m_LastError = fmt::format("Failed to start '{}': {}", WindowsUtils::WideToUtf8(specification.Executable.wstring()), WindowsUtils::GetErrorMessage(createError));
			return false;
		}

		CloseHandle(processInfo.hThread);
		m_ProcessHandle = processInfo.hProcess;
		m_ProcessID = processInfo.dwProcessId;
		m_OutputRead = outputRead;

		if (m_OutputRead)
			StartOutputReader();
		return true;
	}

	void Process::StartOutputReader()
	{
		m_StopReading = false;
		m_ReaderFinished = false;
		m_OutputThread = std::thread([this]()
		{
			char buffer[4096];
			while (!m_StopReading.load())
			{
				DWORD bytesRead = 0;
				if (!ReadFile(static_cast<HANDLE>(m_OutputRead), buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0)
					break; // Broken pipe: every writer has exited, or the read was cancelled

				std::scoped_lock<std::mutex> lock(m_OutputMutex);
				m_Output.append(buffer, bytesRead);
			}
			m_ReaderFinished = true;
		});
	}

	void Process::StopOutputReader(std::chrono::milliseconds gracePeriod)
	{
		if (!m_OutputThread.joinable())
			return;

		const auto deadline = std::chrono::steady_clock::now() + gracePeriod;
		while (!m_ReaderFinished.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(2));

		if (!m_ReaderFinished.load())
		{
			// A grandchild may still hold the pipe open; cancel the blocking read.
			m_StopReading = true;
			CancelSynchronousIo(m_OutputThread.native_handle());
		}
		m_OutputThread.join();
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

	std::string Process::TakeOutput()
	{
		std::scoped_lock<std::mutex> lock(m_OutputMutex);
		return std::exchange(m_Output, std::string());
	}

	void Process::Close()
	{
		StopOutputReader(std::chrono::milliseconds(0));
		if (m_OutputRead)
		{
			CloseHandle(static_cast<HANDLE>(m_OutputRead));
			m_OutputRead = nullptr;
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
		specification.Output = ProcessOutputMode::Capture;

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

		process.StopOutputReader(std::chrono::milliseconds(2000));
		result.ExitCode = exitCode.value_or(-1);
		result.Output = process.TakeOutput();
		return result;
	}

}
