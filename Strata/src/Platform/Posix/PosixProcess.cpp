#include "stpch.h"
#include "Strata/Core/Process.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
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

	namespace
	{

		// A pipe whose parent end (read or write) is closed on exec, so it does not leak into the child or later children.
		bool CreateChildPipe(int (&descriptors)[2], bool parentReads, std::string& error)
		{
			if (pipe(descriptors) != 0)
			{
				error = fmt::format("pipe() failed: {}", std::strerror(errno));
				return false;
			}
			fcntl(parentReads ? descriptors[0] : descriptors[1], F_SETFD, FD_CLOEXEC);
			return true;
		}

		void CloseDescriptor(int& descriptor)
		{
			if (descriptor < 0)
				return;
			close(descriptor);
			descriptor = -1;
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

		if (specification.PipeInput && specification.Output == ProcessOutputMode::Inherit)
		{
			m_LastError = "Piped input needs captured or discarded output";
			return false;
		}

		int outputPipe[2] = { -1, -1 };
		int errorPipe[2] = { -1, -1 };
		int inputPipe[2] = { -1, -1 };
		auto closePipes = [&]()
		{
			for (int* descriptor : { &outputPipe[0], &outputPipe[1], &errorPipe[0], &errorPipe[1], &inputPipe[0], &inputPipe[1] })
				CloseDescriptor(*descriptor);
		};

		const bool captureOutput = specification.Output == ProcessOutputMode::Capture || specification.Output == ProcessOutputMode::CaptureSeparate;
		const bool separateErrors = specification.Output == ProcessOutputMode::CaptureSeparate;
		if ((captureOutput && !CreateChildPipe(outputPipe, true, m_LastError)) || (separateErrors && !CreateChildPipe(errorPipe, true, m_LastError))
			|| (specification.PipeInput && !CreateChildPipe(inputPipe, false, m_LastError)))
		{
			closePipes();
			return false;
		}

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		if (specification.PipeInput)
		{
			posix_spawn_file_actions_adddup2(&actions, inputPipe[0], STDIN_FILENO);
			posix_spawn_file_actions_addclose(&actions, inputPipe[0]);
			posix_spawn_file_actions_addclose(&actions, inputPipe[1]);
		}
		else if (specification.Output != ProcessOutputMode::Inherit)
		{
			posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
		}

		if (captureOutput)
		{
			posix_spawn_file_actions_adddup2(&actions, outputPipe[1], STDOUT_FILENO);
			posix_spawn_file_actions_adddup2(&actions, separateErrors ? errorPipe[1] : outputPipe[1], STDERR_FILENO);
			posix_spawn_file_actions_addclose(&actions, outputPipe[0]);
			posix_spawn_file_actions_addclose(&actions, outputPipe[1]);
			if (separateErrors)
			{
				posix_spawn_file_actions_addclose(&actions, errorPipe[0]);
				posix_spawn_file_actions_addclose(&actions, errorPipe[1]);
			}
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
		// The child has its own copies of its ends (or failed to start).
		CloseDescriptor(outputPipe[1]);
		CloseDescriptor(errorPipe[1]);
		CloseDescriptor(inputPipe[0]);

		if (result != 0)
		{
			closePipes();
			m_LastError = fmt::format("Failed to start '{}': {}", executable, std::strerror(result));
			return false;
		}

		m_ProcessPid = pid;
		m_ProcessID = static_cast<uint32_t>(pid);
		m_OutputRead = std::exchange(outputPipe[0], -1);
		m_ErrorRead = std::exchange(errorPipe[0], -1);
		m_InputWrite = std::exchange(inputPipe[1], -1);
		StartOutputReaders();
		return true;
	}

	void Process::StartOutputReaders()
	{
		m_StopReading = false;
		auto startReader = [this](int readEnd, std::string& target, std::atomic<bool>& finished, std::thread& thread)
		{
			if (readEnd < 0)
				return;
			finished = false;
			thread = std::thread([this, readEnd, &target, &finished]()
			{
				char buffer[4096];
				while (!m_StopReading.load())
				{
					pollfd descriptor = { readEnd, POLLIN, 0 };
					const int ready = poll(&descriptor, 1, 50);
					if (ready < 0)
					{
						if (errno == EINTR)
							continue;
						break;
					}
					if (ready == 0)
						continue;

					const ssize_t bytesRead = read(readEnd, buffer, sizeof(buffer));
					if (bytesRead < 0 && errno == EINTR)
						continue;
					if (bytesRead <= 0)
						break; // EOF: every writer has exited

					std::scoped_lock<std::mutex> lock(m_OutputMutex);
					target.append(buffer, static_cast<size_t>(bytesRead));
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
		for (std::atomic<bool>* finished : { &m_ReaderFinished, &m_ErrorReaderFinished })
		{
			while (!finished->load() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}

		// The readers poll with a short timeout and see the flag (a grandchild may still hold a pipe open).
		m_StopReading = true;
		if (m_OutputThread.joinable())
			m_OutputThread.join();
		if (m_ErrorThread.joinable())
			m_ErrorThread.join();
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

	bool Process::WriteInput(std::string_view data)
	{
		if (m_InputWrite < 0)
			return false;

		// Writing to a pipe whose reader is gone raises SIGPIPE, which would end this process. The signal is blocked on
		// this thread while writing, and one raised by the write (it is directed at the writing thread) is consumed
		// before the previous mask comes back.
		sigset_t pipeSignal;
		sigemptyset(&pipeSignal);
		sigaddset(&pipeSignal, SIGPIPE);
		sigset_t previousMask;
		if (pthread_sigmask(SIG_BLOCK, &pipeSignal, &previousMask) != 0)
			return false;
		sigset_t pendingBefore;
		sigemptyset(&pendingBefore);
		sigpending(&pendingBefore);
		const bool pipeSignalWasPending = sigismember(&pendingBefore, SIGPIPE) == 1;

		bool written = true;
		while (!data.empty())
		{
			const ssize_t count = write(m_InputWrite, data.data(), data.size());
			if (count < 0)
			{
				if (errno == EINTR)
					continue;
				written = false; // EPIPE: the child closed its end
				break;
			}
			data.remove_prefix(static_cast<size_t>(count));
		}

		if (!written && !pipeSignalWasPending)
		{
			sigset_t pending;
			sigemptyset(&pending);
			if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
			{
				int consumed = 0;
				sigwait(&pipeSignal, &consumed);
			}
		}
		pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
		return written;
	}

	void Process::CloseInput()
	{
		CloseDescriptor(m_InputWrite);
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
		CloseDescriptor(m_OutputRead);
		CloseDescriptor(m_ErrorRead);
		m_ProcessPid = -1;
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
