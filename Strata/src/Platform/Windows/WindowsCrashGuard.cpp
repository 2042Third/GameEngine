#include "stpch.h"
#include "Strata/Core/CrashGuard.h"

#include <Windows.h>
#include <malloc.h>

namespace Strata
{

	namespace
	{

		constexpr DWORD c_CppExceptionCode = 0xE06D7363; // MSVC C++ exception ('msc')

		// Plain data only: filled inside the exception filter, which may run on a nearly exhausted stack. It lives in
		// Invoke's frame, so the filter needs no stack for the unwinding context.
		struct FaultRecord
		{
			DWORD Code = 0;
			uint64_t Address = 0;
			ULONG_PTR AccessType = 0;
			ULONG_PTR AccessAddress = 0;
			bool HasAccessInfo = false;
			uint64_t Frames[c_MaxCrashStackFrames]; // The first FrameCount are filled (not cleared: Invoke guards every script call)
			uint32_t FrameCount = 0;
			CONTEXT Unwind; // The context being unwound
		};

#if defined(_M_X64)
		// The faulting instruction and its callers' return addresses, unwound from the fault's context with the functions'
		// unwind data. A corrupted stack may fault while it is read: that ends the walk (a nested exception in the filter
		// would otherwise be dispatched on top of the one being handled).
		void CaptureStack(const CONTEXT* faulting, FaultRecord* record)
		{
			__try
			{
				CONTEXT& context = record->Unwind;
				context = *faulting;
				while (record->FrameCount < c_MaxCrashStackFrames && context.Rip != 0)
				{
					record->Frames[record->FrameCount++] = context.Rip;
					DWORD64 imageBase = 0;
					PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
					if (function)
					{
						PVOID handlerData = nullptr;
						DWORD64 establisherFrame = 0;
						RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisherFrame, nullptr);
					}
					else
					{
						// A leaf function, which has no unwind data: its return address is on top of the stack.
						context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
						context.Rsp += sizeof(DWORD64);
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}
#endif

		int FilterException(EXCEPTION_POINTERS* pointers, FaultRecord* record)
		{
			const EXCEPTION_RECORD* exception = pointers->ExceptionRecord;
			switch (exception->ExceptionCode)
			{
				case EXCEPTION_ACCESS_VIOLATION:
				case EXCEPTION_IN_PAGE_ERROR:
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
				case EXCEPTION_DATATYPE_MISALIGNMENT:
				case EXCEPTION_ILLEGAL_INSTRUCTION:
				case EXCEPTION_PRIV_INSTRUCTION:
				case EXCEPTION_INT_DIVIDE_BY_ZERO:
				case EXCEPTION_INT_OVERFLOW:
				case EXCEPTION_FLT_DIVIDE_BY_ZERO:
				case EXCEPTION_FLT_INVALID_OPERATION:
				case EXCEPTION_FLT_OVERFLOW:
				case EXCEPTION_FLT_UNDERFLOW:
				case EXCEPTION_FLT_INEXACT_RESULT:
				case EXCEPTION_FLT_DENORMAL_OPERAND:
				case EXCEPTION_FLT_STACK_CHECK:
				case EXCEPTION_STACK_OVERFLOW:
				case EXCEPTION_BREAKPOINT:
				case c_CppExceptionCode:
				case c_CrashGuardAbortExceptionCode:
					break;
				default:
					return EXCEPTION_CONTINUE_SEARCH;
			}

			record->Code = exception->ExceptionCode;
			record->Address = reinterpret_cast<uint64_t>(exception->ExceptionAddress);
			if ((exception->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || exception->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && exception->NumberParameters >= 2)
			{
				record->HasAccessInfo = true;
				record->AccessType = exception->ExceptionInformation[0];
				record->AccessAddress = exception->ExceptionInformation[1];
			}
			// After a stack overflow the filter runs on what the guard page left: no stack walk there.
#if defined(_M_X64)
			if (exception->ExceptionCode != EXCEPTION_STACK_OVERFLOW && pointers->ContextRecord)
				CaptureStack(pointers->ContextRecord, record);
#endif
			if (record->FrameCount == 0)
				record->Frames[record->FrameCount++] = record->Address;
			return EXCEPTION_EXECUTE_HANDLER;
		}

		// Must not contain objects with destructors (C2712), hence the separate function.
		bool InvokeWithSEH(CrashGuard::GuardedFunction function, void* userData, FaultRecord* record)
		{
			__try
			{
				function(userData);
				return true;
			}
			__except (FilterException(GetExceptionInformation(), record))
			{
				if (record->Code == EXCEPTION_STACK_OVERFLOW)
					_resetstkoflw(); // Restore the guard page so a later overflow is caught again
				return false;
			}
		}

		std::string DescribeFault(const FaultRecord& record)
		{
			switch (record.Code)
			{
				case EXCEPTION_ACCESS_VIOLATION:
				{
					if (!record.HasAccessInfo)
						return "Access violation";
					const char* operation = record.AccessType == 0 ? "reading" : (record.AccessType == 8 ? "executing" : "writing");
					return fmt::format("Access violation {} address 0x{:016X}", operation, record.AccessAddress);
				}
				case EXCEPTION_IN_PAGE_ERROR:          return "In-page I/O error";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:  return "Array bounds exceeded";
				case EXCEPTION_DATATYPE_MISALIGNMENT:  return "Misaligned data access";
				case EXCEPTION_ILLEGAL_INSTRUCTION:    return "Illegal instruction";
				case EXCEPTION_PRIV_INSTRUCTION:       return "Privileged instruction";
				case EXCEPTION_INT_DIVIDE_BY_ZERO:     return "Integer division by zero";
				case EXCEPTION_INT_OVERFLOW:           return "Integer overflow";
				case EXCEPTION_FLT_DIVIDE_BY_ZERO:     return "Floating-point division by zero";
				case EXCEPTION_FLT_INVALID_OPERATION:  return "Invalid floating-point operation";
				case EXCEPTION_FLT_OVERFLOW:           return "Floating-point overflow";
				case EXCEPTION_FLT_UNDERFLOW:          return "Floating-point underflow";
				case EXCEPTION_FLT_INEXACT_RESULT:     return "Inexact floating-point result";
				case EXCEPTION_FLT_DENORMAL_OPERAND:   return "Denormal floating-point operand";
				case EXCEPTION_FLT_STACK_CHECK:        return "Floating-point stack check";
				case EXCEPTION_STACK_OVERFLOW:         return "Stack overflow";
				case EXCEPTION_BREAKPOINT:             return "Breakpoint hit without a debugger attached";
				case c_CppExceptionCode:               return "Unhandled C++ exception";
				case c_CrashGuardAbortExceptionCode:   return "abort() called (for example by a failed assertion)";
			}
			return fmt::format("Structured exception 0x{:08X}", record.Code);
		}

	}

	bool CrashGuard::Invoke(GuardedFunction function, void* userData, CrashInfo* outInfo)
	{
		FaultRecord record;
		if (InvokeWithSEH(function, userData, &record))
			return true;

		if (outInfo)
		{
			outInfo->Description = fmt::format("{} at 0x{:016X}", DescribeFault(record), record.Address);
			outInfo->Code = record.Code;
			outInfo->Address = record.Address;
			outInfo->Stack.assign(record.Frames, record.Frames + record.FrameCount);
		}
		return false;
	}

}
