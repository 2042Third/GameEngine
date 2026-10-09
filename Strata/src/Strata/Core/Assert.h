#pragma once

#include "Strata/Core/Base.h"

#include <spdlog/fmt/fmt.h>

#include <string>

namespace Strata
{

	struct AssertInfo
	{
		bool IsCoreAssert = true;
		bool IsVerify = false;
		const char* Condition = "";
		const char* File = "";
		int Line = 0;
		std::string Message;
	};

	enum class AssertAction
	{
		Break,    // Stop in the debugger at the assert site
		Abort,    // Terminate the process
		Continue  // Ignore the failure and keep running (used by tests)
	};

	using AssertHandler = AssertAction (*)(const AssertInfo& info);

	// Overrides how failed assertions are handled (pass nullptr to restore the default handler).
	// The default handler logs the failure, breaks if a debugger is attached, and aborts otherwise.
	void SetAssertHandler(AssertHandler handler);

	namespace Detail
	{
		AssertAction HandleAssertFailure(bool isCore, bool isVerify, const char* condition, const char* file, int line, std::string message);

		inline std::string FormatAssertMessage()
		{
			return {};
		}

		template<typename... Args>
		std::string FormatAssertMessage(fmt::format_string<Args...> format, Args&&... args)
		{
			return fmt::format(format, std::forward<Args>(args)...);
		}
	}

}

#define ST_INTERNAL_ASSERT_IMPL(isCore, isVerify, condition, ...) \
	do \
	{ \
		if (!(condition)) [[unlikely]] \
		{ \
			if (::Strata::Detail::HandleAssertFailure(isCore, isVerify, #condition, __FILE__, __LINE__, ::Strata::Detail::FormatAssertMessage(__VA_ARGS__)) == ::Strata::AssertAction::Break) \
				ST_DEBUGBREAK(); \
		} \
	} while (false)

// Asserts are compiled out of Dist builds; the condition must not have side effects.
#ifdef ST_ENABLE_ASSERTS
	#define ST_CORE_ASSERT(condition, ...) ST_INTERNAL_ASSERT_IMPL(true, false, condition, __VA_ARGS__)
	#define ST_ASSERT(condition, ...) ST_INTERNAL_ASSERT_IMPL(false, false, condition, __VA_ARGS__)
#else
	#define ST_CORE_ASSERT(condition, ...) ((void)0)
	#define ST_ASSERT(condition, ...) ((void)0)
#endif

// Verifies are always evaluated, including Dist builds.
#define ST_CORE_VERIFY(condition, ...) ST_INTERNAL_ASSERT_IMPL(true, true, condition, __VA_ARGS__)
#define ST_VERIFY(condition, ...) ST_INTERNAL_ASSERT_IMPL(false, true, condition, __VA_ARGS__)
