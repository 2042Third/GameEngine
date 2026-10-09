#include "stpch.h"
#include "Strata/Core/Assert.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace Strata
{

	static std::atomic<AssertHandler> s_AssertHandler = nullptr;

	static AssertAction DefaultAssertHandler(const AssertInfo&)
	{
		if (Platform::IsDebuggerAttached())
			return AssertAction::Break;

		return AssertAction::Abort;
	}

	void SetAssertHandler(AssertHandler handler)
	{
		s_AssertHandler.store(handler);
	}

	namespace Detail
	{

		AssertAction HandleAssertFailure(bool isCore, bool isVerify, const char* condition, const char* file, int line, std::string message)
		{
			AssertInfo info;
			info.IsCoreAssert = isCore;
			info.IsVerify = isVerify;
			info.Condition = condition;
			info.File = file;
			info.Line = line;
			info.Message = std::move(message);

			const char* kind = isVerify ? "Verify" : "Assertion";
			if (Log::IsInitialized())
			{
				auto& logger = isCore ? Log::GetCoreLogger() : Log::GetClientLogger();
				if (info.Message.empty())
					logger->critical("{} '{}' failed at {}:{}", kind, condition, file, line);
				else
					logger->critical("{} '{}' failed at {}:{}: {}", kind, condition, file, line, info.Message);
				logger->flush();
			}
			else
			{
				std::fprintf(stderr, "%s '%s' failed at %s:%d: %s\n", kind, condition, file, line, info.Message.c_str());
				std::fflush(stderr);
			}

			AssertHandler handler = s_AssertHandler.load();
			const AssertAction action = handler ? handler(info) : DefaultAssertHandler(info);
			if (action == AssertAction::Abort)
			{
				Log::Shutdown();
				std::abort();
			}
			return action;
		}

	}

}
