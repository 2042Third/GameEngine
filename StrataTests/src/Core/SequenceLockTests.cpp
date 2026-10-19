#include <doctest/doctest.h>

#include "Strata/Core/SequenceLock.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

using namespace Strata;

namespace
{

	// Values whose parts can be checked against each other: a torn value mixes parts of two of them.
	struct Sample
	{
		float X = 0.0f;
		float Y = 0.0f;
		float Z = 0.0f;
		uint64_t Index = 0;
	};

	Sample MakeSample(uint64_t index)
	{
		const float value = static_cast<float>(index);
		return Sample { value, value * 2.0f, value * 3.0f, index };
	}

	bool IsConsistent(const Sample& sample)
	{
		const Sample expected = MakeSample(sample.Index);
		return sample.X == expected.X && sample.Y == expected.Y && sample.Z == expected.Z;
	}

}

TEST_SUITE("Core.SequenceLock")
{
	TEST_CASE("A value is taken once, and only after it was published")
	{
		SequenceLockedValue<Sample> value;
		Sample taken;
		CHECK_FALSE(value.TakeNew(taken));

		value.Publish(MakeSample(1));
		value.Publish(MakeSample(2));
		REQUIRE(value.TakeNew(taken));
		CHECK(taken.Index == 2); // The latest one
		CHECK(IsConsistent(taken));
		CHECK_FALSE(value.TakeNew(taken));

		value.Publish(MakeSample(3));
		REQUIRE(value.TakeNew(taken));
		CHECK(taken.Index == 3);
	}

	TEST_CASE("Values handed across threads are never torn and the latest one arrives")
	{
		// Exact in float up to 2^24, so the parts of every sample are exact.
		constexpr uint64_t c_SampleCount = 2'000'000;
		SequenceLockedValue<Sample> value;
		std::atomic<bool> published = false;
		std::atomic<uint64_t> takenSoFar = 0;
		std::thread writer([&]()
		{
			for (uint64_t index = 1; index <= c_SampleCount; index++)
			{
				value.Publish(MakeSample(index));
				// Halfway, until the reader took a sample: on a busy machine (parallel tests) the writer could otherwise publish
				// everything before the reader first runs, and the reader would see only the last sample.
				if (index == c_SampleCount / 2)
				{
					const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
					while (takenSoFar.load() == 0 && std::chrono::steady_clock::now() < deadline)
						std::this_thread::yield();
				}
			}
			published = true;
		});

		uint64_t takenCount = 0;
		uint64_t tornCount = 0;
		uint64_t outOfOrderCount = 0;
		uint64_t last = 0;
		while (true)
		{
			// Read before taking: once everything is published, a take that finds nothing new means the last one was taken.
			const bool finished = published.load();
			Sample sample;
			if (value.TakeNew(sample))
			{
				takenCount++;
				takenSoFar.store(takenCount);
				if (!IsConsistent(sample))
					tornCount++;
				if (sample.Index <= last)
					outOfOrderCount++;
				last = sample.Index;
			}
			else if (finished)
			{
				break;
			}
		}
		writer.join();

		CHECK(tornCount == 0);
		CHECK(outOfOrderCount == 0);
		CHECK(last == c_SampleCount);
		CHECK(takenCount > 1);
		MESSAGE("Samples taken while ", c_SampleCount, " were published: ", takenCount);
	}
}
