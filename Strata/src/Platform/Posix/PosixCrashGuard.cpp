#include "stpch.h"
#include "Strata/Core/CrashGuard.h"

#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string_view>
#include <unistd.h>

#if defined(__GLIBCXX__)
	#include <cxxabi.h>
#endif

namespace Strata
{

	namespace
	{

		struct GuardFrame
		{
			sigjmp_buf JumpBuffer;
			GuardFrame* Previous = nullptr;
			volatile sig_atomic_t Signal = 0;
			void* volatile FaultAddress = nullptr;
		};

		// SIGABRT is not contained, only reported (see AbortFromGuardedCode).
		constexpr int c_GuardedSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT };
		constexpr size_t c_AlternateStackSize = 64 * 1024;

		// Whether sigsetjmp saves the signal mask for siglongjmp to restore, which costs a system call per guarded call.
#if defined(ST_PLATFORM_MACOS)
		// macOS: only the mask-restoring siglongjmp also clears the kernel's record that the thread runs on the alternate
		// signal stack. Without it, signals after a fault would no longer switch to that stack, and a second stack
		// overflow could not be handled.
		constexpr int c_SaveSignalMask = 1;
#else
		// Linux tells from the stack pointer whether a thread runs on the alternate stack, so jumping out of the handler
		// needs no cleanup. The one mask change to undo is the delivered signal, which the kernel blocks while its handler
		// runs (sa_mask is empty and SA_NODEFER is not set); Invoke unblocks it on the fault path. Nested guards are no
		// different: each fault path undoes the block of the signal it handled.
		constexpr int c_SaveSignalMask = 0;
#endif

		thread_local GuardFrame* t_CurrentFrame = nullptr;
		thread_local bool t_AlternateStackInstalled = false;

		struct sigaction s_PreviousActions[NSIG];
		std::once_flag s_InstallOnce;

		// Async-signal-safe: write(2) only.
		void WriteToStandardError(std::string_view text)
		{
			const int savedErrno = errno;
			while (!text.empty())
			{
				const ssize_t written = write(STDERR_FILENO, text.data(), text.size());
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					break;
				text.remove_prefix(static_cast<size_t>(written));
			}
			errno = savedErrno;
		}

		// A signal no guard handles goes where it would have gone without the guard. A handler installed before is called
		// directly, so the guard keeps its handlers for later guarded calls (the previous handler's own signal mask and
		// flags are not applied). With the default action, or when the signal was ignored, that disposition is restored
		// for good: the process ends (or ignores the signal) either way. Returning re-executes a faulting instruction,
		// which then reaches the restored disposition; a trap or abort() does not happen again by returning, so it is
		// raised again - it stays blocked until this handler returns, and is delivered then.
		void ForwardToPreviousHandler(int signal, siginfo_t* info, void* context)
		{
			const struct sigaction& previous = s_PreviousActions[signal];
			if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN)
			{
				if ((previous.sa_flags & SA_SIGINFO) != 0)
					previous.sa_sigaction(signal, info, context);
				else
					previous.sa_handler(signal);
				return;
			}
			sigaction(signal, &previous, nullptr);
			if (signal == SIGTRAP || signal == SIGABRT)
				raise(signal);
		}

		// Sent by another process (kill, sigqueue) rather than raised by the code running on this thread: not a fault of
		// a guarded call, even when one runs.
		bool IsSentByAnotherProcess(const siginfo_t* info)
		{
			if (!info)
				return false;
			bool sent = info->si_code == SI_USER || info->si_code == SI_QUEUE;
#if defined(SI_TKILL)
			sent = sent || info->si_code == SI_TKILL;
#endif
			return sent && info->si_pid != getpid();
		}

		// abort() in guarded code cannot be contained: the C library also aborts this way when it detects heap corruption,
		// while it holds allocator locks that jumping out of the handler would never release - the thread's next
		// allocation would then block forever, freezing the program without any report. A failed assert() or
		// std::terminate cannot be told apart from that, so every abort is reported (with async-signal-safe calls only)
		// and the process ends the way abort() ends it.
		void AbortFromGuardedCode(siginfo_t* info, void* context)
		{
			WriteToStandardError("Strata: guarded code (a script) called abort() - a failed assertion, std::terminate, or an error the C "
				"library detected such as heap corruption. An abort cannot be contained safely on this platform; the process ends.\n");
			ForwardToPreviousHandler(SIGABRT, info, context);
		}

		void SignalHandler(int signal, siginfo_t* info, void* context)
		{
			GuardFrame* frame = t_CurrentFrame;
			if (frame && !IsSentByAnotherProcess(info))
			{
				if (signal == SIGABRT)
				{
					AbortFromGuardedCode(info, context);
					return;
				}
				frame->Signal = signal;
				frame->FaultAddress = info ? info->si_addr : nullptr;
				t_CurrentFrame = frame->Previous;
				siglongjmp(frame->JumpBuffer, 1);
			}
			ForwardToPreviousHandler(signal, info, context);
		}

		void InstallHandlers()
		{
			struct sigaction action = {};
			action.sa_sigaction = SignalHandler;
			action.sa_flags = SA_SIGINFO | SA_ONSTACK;
			sigemptyset(&action.sa_mask);
			for (int signal : c_GuardedSignals)
				sigaction(signal, &action, &s_PreviousActions[signal]);
		}

		void EnsureAlternateStack()
		{
			if (t_AlternateStackInstalled)
				return;

			// Required to run the handler after a stack overflow. Intentionally leaked: the stack must stay
			// valid for the remaining lifetime of the thread.
			stack_t stack = {};
			stack.ss_sp = std::malloc(c_AlternateStackSize);
			stack.ss_size = c_AlternateStackSize;
			stack.ss_flags = 0;
			if (stack.ss_sp && sigaltstack(&stack, nullptr) == 0)
				t_AlternateStackInstalled = true;
		}

		// Runs the guarded function; false if a C++ exception escaped it. Exceptions must not unwind through Invoke (they
		// would leave the guard frame registered), and keeping the handler out of Invoke keeps exception handling and
		// sigsetjmp in separate frames.
		bool CallCatchingExceptions(CrashGuard::GuardedFunction function, void* userData, [[maybe_unused]] GuardFrame& frame)
		{
			try
			{
				function(userData);
				return true;
			}
#if defined(__GLIBCXX__)
			catch (abi::__forced_unwind&)
			{
				// Thread cancellation (pthread_cancel, pthread_exit) unwinds the thread with this exception, which must go on:
				// the C library ends the process if it is swallowed. The frame is unregistered first, as Invoke would.
				t_CurrentFrame = frame.Previous;
				throw;
			}
#endif
			catch (...)
			{
				return false;
			}
		}

		std::string DescribeSignal(int signal, void* address)
		{
			switch (signal)
			{
				case SIGSEGV: return fmt::format("Segmentation fault accessing address {}", address);
				case SIGBUS:  return fmt::format("Bus error accessing address {}", address);
				case SIGFPE:  return "Arithmetic exception (e.g. integer division by zero)";
				case SIGILL:  return "Illegal instruction";
				case SIGTRAP: return "Breakpoint trap without a debugger attached";
			}
			return fmt::format("Signal {}", signal);
		}

	}

	bool CrashGuard::Invoke(GuardedFunction function, void* userData, CrashInfo* outInfo)
	{
		std::call_once(s_InstallOnce, InstallHandlers);
		EnsureAlternateStack();

		GuardFrame frame;
		frame.Previous = t_CurrentFrame;
		if (sigsetjmp(frame.JumpBuffer, c_SaveSignalMask) == 0)
		{
			t_CurrentFrame = &frame;
			const bool returned = CallCatchingExceptions(function, userData, frame);
			t_CurrentFrame = frame.Previous;
			if (returned)
				return true;

			// An escaping exception is a fault of the guarded code, as on Windows (where it is a structured exception).
			if (outInfo)
			{
				outInfo->Description = "Unhandled C++ exception";
				outInfo->Code = 0;
				outInfo->Address = 0;
			}
			return false;
		}

		// Arrived here through siglongjmp from the signal handler (which already popped the frame).
		if constexpr (c_SaveSignalMask == 0)
		{
			// The handler ran with the delivered signal blocked and the mask was not restored: unblock it, or the next fault
			// of the same kind could not be delivered (the kernel kills a process whose fault signal is blocked).
			sigset_t delivered;
			sigemptyset(&delivered);
			sigaddset(&delivered, frame.Signal);
			pthread_sigmask(SIG_UNBLOCK, &delivered, nullptr);
		}

		if (outInfo)
		{
			outInfo->Description = DescribeSignal(frame.Signal, frame.FaultAddress);
			outInfo->Code = static_cast<uint64_t>(frame.Signal);
			outInfo->Address = reinterpret_cast<uint64_t>(frame.FaultAddress);
		}
		return false;
	}

}
