#include "stpch.h"
#include "Strata/Core/Process.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(ST_PLATFORM_MACOS)
	#include <crt_externs.h>
	#define ST_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
	#define ST_ENVIRON environ
#endif

namespace Strata
{

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

		int pipeFds[2] = { -1, -1 };
		if (specification.Output == ProcessOutputMode::Capture)
		{
			if (pipe(pipeFds) != 0)
			{
				m_LastError = fmt::format("pipe() failed: {}", std::strerror(errno));
				return false;
			}
			fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
		}

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		if (specification.Output != ProcessOutputMode::Inherit)
			posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

		if (specification.Output == ProcessOutputMode::Capture)
		{
			posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
			posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
			posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
			posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
		}
		else if (specification.Output == ProcessOutputMode::Discard)
		{
			posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
			posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
		}

		const std::string workingDirectory = specification.WorkingDirectory.string();
		if (!workingDirectory.empty())
			posix_spawn_file_actions_addchdir_np(&actions, workingDirectory.c_str());

		posix_spawnattr_t attributes;
		posix_spawnattr_init(&attributes);
		if (specification.Detached)
		{
			posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
			posix_spawnattr_setpgroup(&attributes, 0);
		}

		const std::string executable = specification.Executable.string();
		std::vector<std::string> arguments;
		arguments.reserve(specification.Arguments.size() + 1);
		arguments.push_back(executable);
		arguments.insert(arguments.end(), specification.Arguments.begin(), specification.Arguments.end());

		std::vector<char*> argv;
		argv.reserve(arguments.size() + 1);
		for (std::string& argument : arguments)
			argv.push_back(argument.data());
		argv.push_back(nullptr);

		pid_t pid = -1;
		const int result = posix_spawnp(&pid, executable.c_str(), &actions, &attributes, argv.data(), ST_ENVIRON);

		posix_spawn_file_actions_destroy(&actions);
		posix_spawnattr_destroy(&attributes);
		if (pipeFds[1] >= 0)
			close(pipeFds[1]);

		if (result != 0)
		{
			if (pipeFds[0] >= 0)
				close(pipeFds[0]);
			m_LastError = fmt::format("Failed to start '{}': {}", executable, std::strerror(result));
			return false;
		}

		m_ProcessPid = pid;
		m_ProcessID = static_cast<uint32_t>(pid);
		m_OutputRead = pipeFds[0];
		if (m_OutputRead >= 0)
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
				pollfd descriptor = { m_OutputRead, POLLIN, 0 };
				const int ready = poll(&descriptor, 1, 50);
				if (ready < 0)
				{
					if (errno == EINTR)
						continue;
					break;
				}
				if (ready == 0)
					continue;

				const ssize_t bytesRead = read(m_OutputRead, buffer, sizeof(buffer));
				if (bytesRead < 0 && errno == EINTR)
					continue;
				if (bytesRead <= 0)
					break; // EOF: every writer has exited

				std::scoped_lock<std::mutex> lock(m_OutputMutex);
				m_Output.append(buffer, static_cast<size_t>(bytesRead));
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

		m_StopReading = true;
		m_OutputThread.join();
	}

	bool Process::IsRunning()
	{
		return m_ProcessPid > 0 && !Wait(std::chrono::milliseconds(0)).has_value();
	}

	std::optional<int> Process::Wait(std::optional<std::chrono::milliseconds> timeout)
	{
		if (m_ExitCode)
			return m_ExitCode;
		if (m_ProcessPid <= 0)
			return std::nullopt;

		const auto deadline = std::chrono::steady_clock::now() + timeout.value_or(std::chrono::milliseconds(0));
		while (true)
		{
			int status = 0;
			const pid_t waited = waitpid(m_ProcessPid, &status, timeout ? WNOHANG : 0);
			if (waited == m_ProcessPid)
			{
				if (WIFEXITED(status))
					m_ExitCode = WEXITSTATUS(status);
				else if (WIFSIGNALED(status))
					m_ExitCode = 128 + WTERMSIG(status);
				else
					m_ExitCode = -1;
				return m_ExitCode;
			}

			if (waited < 0 && errno != EINTR)
			{
				m_ExitCode = -1;
				return m_ExitCode;
			}

			if (timeout && std::chrono::steady_clock::now() >= deadline)
				return std::nullopt;

			if (timeout)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
	}

	bool Process::Terminate()
	{
		if (m_ProcessPid <= 0 || m_ExitCode)
			return false;

		if (kill(m_ProcessPid, SIGKILL) != 0)
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
		if (m_OutputRead >= 0)
		{
			close(m_OutputRead);
			m_OutputRead = -1;
		}
		m_ProcessPid = -1;
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
