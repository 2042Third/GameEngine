#include "stpch.h"
#include "Strata/Core/CrashGuard.h"

#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <pthread.h>

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

		// SIGABRT: abort() raises it (a failed assert(), std::abort(), a stack protector or a C++ runtime ending in abort).
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

		void SignalHandler(int signal, siginfo_t* info, void*)
		{
			GuardFrame* frame = t_CurrentFrame;
			if (frame)
			{
				frame->Signal = signal;
				frame->FaultAddress = info ? info->si_addr : nullptr;
				t_CurrentFrame = frame->Previous;
				siglongjmp(frame->JumpBuffer, 1);
			}

			// Not inside a guard: restore the previous disposition. Returning re-executes the faulting
			// instruction, which then reaches the previous handler (or the default action). A trap or abort()
			// does not happen again by returning, so it is raised again: the signal stays blocked until this
			// handler returns, and is then delivered to the restored disposition.
			sigaction(signal, &s_PreviousActions[signal], nullptr);
			if (signal == SIGTRAP || signal == SIGABRT)
				raise(signal);
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
		bool CallCatchingExceptions(CrashGuard::GuardedFunction function, void* userData)
		{
			try
			{
				function(userData);
				return true;
			}
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
				case SIGABRT: return "abort() called (for example by a failed assertion)";
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
			const bool returned = CallCatchingExceptions(function, userData);
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
