#include "stpch.h"
#include "Strata/Core/CrashGuard.h"

#include "Strata/Core/Log.h"

#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <pthread.h>
#include <string_view>
#include <unistd.h>

#if defined(__GLIBCXX__)
	#include <cxxabi.h>
#endif

#if defined(ST_PLATFORM_MACOS)
	#include <sys/ucontext.h>
#endif

namespace Strata
{

	// Leaving the signal handler after a contained fault. The handler runs on the thread's alternate signal stack (so it
	// can run after a stack overflow), and the kernel delivers a later signal on that stack only if it knows that the
	// thread has left it:
	//  - Linux tells from the stack pointer, so the handler jumps straight back into Invoke (siglongjmp).
	//  - macOS keeps a per-thread "on the alternate stack" flag that sigreturn - returning from the handler - resets to
	//    what it was before the signal. Jumping out leaves it set (the arm64 longjmp does not reset it), and the kernel
	//    then delivers the next signal on the interrupted stack; after a stack overflow it cannot, and it ends the process
	//    with SIGILL instead. So the handler rewrites the interrupted thread state to continue in ResumeAfterFault on a
	//    small stack of its own and returns; ResumeAfterFault then jumps back into Invoke from ordinary code.
	// Either way sigsetjmp does not save the signal mask (that would cost a system call per guarded call); Invoke unblocks
	// the delivered signal on the fault path (on macOS sigreturn has already restored the mask).

	namespace
	{

		struct GuardFrame
		{
			sigjmp_buf JumpBuffer;
			GuardFrame* Previous = nullptr;
			volatile sig_atomic_t Signal = 0;
			void* volatile FaultAddress = nullptr;
#if defined(ST_PLATFORM_MACOS)
			// End of this thread's recovery stack (see ResumeAfterReturn), copied here so the handler reads no thread-local
			// state beyond the current frame.
			void* RecoveryStackTop = nullptr;
#endif
		};

		// SIGABRT is not contained, only reported (see AbortFromGuardedCode).
		constexpr int c_GuardedSignals[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT };
		constexpr size_t c_AlternateStackSize = 64 * 1024;
#if defined(ST_PLATFORM_MACOS)
		// ResumeAfterFault only calls siglongjmp.
		constexpr size_t c_RecoveryStackSize = 16 * 1024;
#endif

		thread_local GuardFrame* t_CurrentFrame = nullptr;
		thread_local bool t_AlternateStackInstalled = false;
#if defined(ST_PLATFORM_MACOS)
		thread_local void* t_RecoveryStackTop = nullptr;
#endif

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

#if defined(ST_PLATFORM_MACOS)
		// Where a contained fault continues once the handler has returned: on the recovery stack, as if called with the
		// faulted guard's frame as its argument (it has no caller to return to).
		[[noreturn]] void ResumeAfterFault(GuardFrame* frame)
		{
			siglongjmp(frame->JumpBuffer, 1);
		}

		// Makes the interrupted thread continue in ResumeAfterFault(frame) when the handler returns, on the recovery stack:
		// the stack it was interrupted on may be exhausted. False if that cannot be arranged; the handler then has to jump
		// out directly.
		bool ResumeAfterReturn(void* context, GuardFrame* frame)
		{
			ucontext_t* interrupted = static_cast<ucontext_t*>(context);
			if (!interrupted || !interrupted->uc_mcontext || !frame->RecoveryStackTop)
				return false;

			// Aligned as both ABIs require for the stack pointer at a call (unused on other architectures).
			[[maybe_unused]] const uintptr_t stackTop = reinterpret_cast<uintptr_t>(frame->RecoveryStackTop) & ~static_cast<uintptr_t>(15);
	#if defined(__aarch64__)
			// The SDK's accessors also work where pointer authentication makes the registers opaque (arm64e).
			auto& state = interrupted->uc_mcontext->__ss;
			state.__x[0] = reinterpret_cast<uint64_t>(frame);
		#if defined(__darwin_arm_thread_state64_set_sp)
			__darwin_arm_thread_state64_set_sp(state, stackTop);
		#else
			state.__sp = stackTop;
		#endif
		#if defined(__darwin_arm_thread_state64_set_pc_fptr)
			__darwin_arm_thread_state64_set_pc_fptr(state, &ResumeAfterFault);
		#else
			state.__pc = reinterpret_cast<uint64_t>(&ResumeAfterFault);
		#endif
			return true;
	#elif defined(__x86_64__)
			// At a function's entry the stack pointer is 8 below a 16-byte boundary, where the call left its return address;
			// this entry has none (zero ends stack walks there).
			const uintptr_t entryStackPointer = stackTop - sizeof(uint64_t);
			*reinterpret_cast<uint64_t*>(entryStackPointer) = 0;
			auto& state = interrupted->uc_mcontext->__ss;
			state.__rdi = reinterpret_cast<uint64_t>(frame);
			state.__rsp = entryStackPointer;
			state.__rip = reinterpret_cast<uint64_t>(&ResumeAfterFault);
			return true;
	#else
			return false;
	#endif
		}
#endif

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
#if defined(ST_PLATFORM_MACOS)
				if (ResumeAfterReturn(context, frame))
					return;
#endif
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

			// Required to run the handler after a stack overflow. Intentionally leaked: the stacks must stay valid for the
			// remaining lifetime of the thread.
			void* alternateStack = std::malloc(c_AlternateStackSize);
#if defined(ST_PLATFORM_MACOS)
			// Without a recovery stack the handler would have to jump out and leave the thread marked as running on the
			// alternate stack (see above), so the alternate stack is installed only together with one.
			void* recoveryStack = std::malloc(c_RecoveryStackSize);
			if (!recoveryStack)
			{
				std::free(alternateStack);
				return;
			}
#endif
			stack_t stack = {};
			stack.ss_sp = alternateStack;
			stack.ss_size = c_AlternateStackSize;
			stack.ss_flags = 0;
			if (!alternateStack || sigaltstack(&stack, nullptr) != 0)
			{
				std::free(alternateStack);
#if defined(ST_PLATFORM_MACOS)
				std::free(recoveryStack);
#endif
				return;
			}
			t_AlternateStackInstalled = true;
#if defined(ST_PLATFORM_MACOS)
			t_RecoveryStackTop = static_cast<char*>(recoveryStack) + c_RecoveryStackSize;
#endif
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
#if defined(ST_PLATFORM_MACOS)
		frame.RecoveryStackTop = t_RecoveryStackTop;
#endif
		if (sigsetjmp(frame.JumpBuffer, 0) == 0)
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

		// Arrived here after a fault (the handler already popped the frame). Where the handler jumped out, it ran with the
		// delivered signal blocked and the mask was not restored: unblock it, or the next fault of the same kind could not
		// be delivered (the kernel kills a process whose fault signal is blocked). Nested guards are no different: each
		// fault path undoes the block of the signal it handled.
		sigset_t delivered;
		sigemptyset(&delivered);
		sigaddset(&delivered, frame.Signal);
		pthread_sigmask(SIG_UNBLOCK, &delivered, nullptr);

		// The thread must have left its alternate signal stack, or the next fault could not be contained (see above).
		stack_t signalStack = {};
		if (sigaltstack(nullptr, &signalStack) == 0 && (signalStack.ss_flags & SS_ONSTACK) != 0)
			ST_CORE_ERROR("CrashGuard: this thread still counts as running on its alternate signal stack after a contained fault; a stack overflow on it can no longer be contained");

		if (outInfo)
		{
			outInfo->Description = DescribeSignal(frame.Signal, frame.FaultAddress);
			outInfo->Code = static_cast<uint64_t>(frame.Signal);
			outInfo->Address = reinterpret_cast<uint64_t>(frame.FaultAddress);
		}
		return false;
	}

}
