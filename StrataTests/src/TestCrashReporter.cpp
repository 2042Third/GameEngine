#include <doctest/doctest.h>

#include <Strata/Core/PlatformDetection.h>

#include <algorithm>
#include <cstring>
#include <utility>

#if !defined(ST_PLATFORM_WINDOWS)
	#include <signal.h>
	#include <unistd.h>
#endif

// The tests run without doctest's POSIX signal handling (it would replace the script crash guard's handlers, see
// DOCTEST_CONFIG_NO_POSIX_SIGNALS), so a crash outside a guarded call would end the run without naming the test. This
// listener remembers the running test case, and a fatal signal handler prints it before the signal's default action
// ends the process. The handler is installed during static initialization, before any crash guard: the guard passes
// faults outside guarded calls on to it. On Windows doctest's own crash reporting names the test.
namespace
{

	// Written by the main thread when a test case starts, read by the signal handler; a torn read only garbles the
	// diagnostic.
	char s_CurrentTestCase[512] = {};

	void SetCurrentTestCase(const char* suite, const char* name)
	{
		size_t length = 0;
		auto append = [&length](const char* text)
		{
			const size_t available = sizeof(s_CurrentTestCase) - 1 - length;
			const size_t count = text ? std::min(std::strlen(text), available) : 0;
			std::memcpy(s_CurrentTestCase + length, text, count);
			length += count;
		};
		if (suite && *suite)
		{
			append(suite);
			append(" / ");
		}
		append(name);
		s_CurrentTestCase[length] = '\0';
	}

	struct CurrentTestCaseListener final : doctest::IReporter
	{
		explicit CurrentTestCaseListener(const doctest::ContextOptions&) {}

		void report_query(const doctest::QueryData&) override {}
		void test_run_start() override {}
		void test_run_end(const doctest::TestRunStats&) override {}
		void test_case_start(const doctest::TestCaseData& data) override { SetCurrentTestCase(data.m_test_suite, data.m_name); }
		void test_case_reenter(const doctest::TestCaseData&) override {}
		void test_case_end(const doctest::CurrentTestCaseStats&) override {}
		void test_case_exception(const doctest::TestCaseException&) override {}
		void subcase_start(const doctest::SubcaseSignature&) override {}
		void subcase_end() override {}
		void log_assert(const doctest::AssertData&) override {}
		void log_message(const doctest::MessageData&) override {}
		void test_case_skipped(const doctest::TestCaseData&) override {}
	};

#if !defined(ST_PLATFORM_WINDOWS)
	void WriteToStandardError(const char* text)
	{
		// Async-signal-safe output; a failed write leaves nothing else to do.
		if (write(STDERR_FILENO, text, std::strlen(text)) < 0)
			return;
	}

	void ReportFatalSignal(int signal)
	{
		WriteToStandardError("\nFatal signal ");
		char number[16] = {};
		int value = signal;
		int length = 0;
		do
		{
			number[length++] = static_cast<char>('0' + value % 10);
			value /= 10;
		} while (value > 0 && length < 15);
		for (int index = 0; index < length / 2; index++)
			std::swap(number[index], number[length - 1 - index]);
		WriteToStandardError(number);
		WriteToStandardError(" in test case: ");
		WriteToStandardError(s_CurrentTestCase[0] ? s_CurrentTestCase : "(none)");
		WriteToStandardError("\n");
		// SA_RESETHAND restored the default action: raising again ends the process with the same signal.
		raise(signal);
	}

	bool InstallFatalSignalReporter()
	{
		struct sigaction action = {};
		action.sa_handler = ReportFatalSignal;
		sigemptyset(&action.sa_mask);
		action.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
		for (int signal : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT })
			sigaction(signal, &action, nullptr);
		return true;
	}

	[[maybe_unused]] const bool s_FatalSignalReporterInstalled = InstallFatalSignalReporter();
#endif

}

REGISTER_LISTENER("current-test-case", 1, CurrentTestCaseListener);
