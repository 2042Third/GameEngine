#include <doctest/doctest.h>

#include "Strata/Core/ErrorThrottle.h"
#include "Strata/Core/Log.h"

#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// Error entries logged since `after` whose message contains `text`.
	size_t CountErrors(uint64_t after, const std::string& text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(after))
			count += entry.Level == LogLevel::Error && entry.Message.find(text) != std::string::npos ? 1 : 0;
		return count;
	}

}

TEST_SUITE("Core.ErrorThrottle")
{
	TEST_CASE("Repeated errors are logged once until cleared")
	{
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		ErrorThrottle errors("ThrottleTest");
		CHECK(errors.Report("device lost"));
		for (int frame = 0; frame < 10; frame++)
			CHECK_FALSE(errors.Report("device lost"));
		CHECK(CountErrors(before, "ThrottleTest: device lost") == 1);
		CHECK(errors.GetLastMessage() == "device lost");

		// A different message is news; going back to the first one is news again.
		CHECK(errors.Report("out of memory"));
		CHECK(errors.Report("device lost"));
		CHECK(CountErrors(before, "ThrottleTest: device lost") == 2);
		CHECK(CountErrors(before, "ThrottleTest: out of memory") == 1);

		// After a success the same failure is reported again.
		errors.Clear();
		CHECK(errors.GetLastMessage().empty());
		CHECK(errors.Report("device lost"));
		CHECK(CountErrors(before, "ThrottleTest: device lost") == 3);
	}
}
