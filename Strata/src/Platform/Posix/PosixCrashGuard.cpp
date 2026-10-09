#include "stpch.h"
#include "Strata/Core/CrashGuard.h"

#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <mutex>

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

		constexpr int c_GuardedSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP };
		constexpr size_t c_AlternateStackSize = 64 * 1024;

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
			// instruction, which then reaches the previous handler (or the default action).
			sigaction(signal, &s_PreviousActions[signal], nullptr);
			if (signal == SIGTRAP)
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
		if (sigsetjmp(frame.JumpBuffer, 1) == 0)
		{
			t_CurrentFrame = &frame;
			function(userData);
			t_CurrentFrame = frame.Previous;
			return true;
		}

		// Arrived here through siglongjmp from the signal handler (which already popped the frame).
		if (outInfo)
		{
			outInfo->Description = DescribeSignal(frame.Signal, frame.FaultAddress);
			outInfo->Code = static_cast<uint64_t>(frame.Signal);
			outInfo->Address = reinterpret_cast<uint64_t>(frame.FaultAddress);
		}
		return false;
	}

}
