#include <doctest/doctest.h>

#include "Strata/Core/CommandLine.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/Layer.h"
#include "Strata/Core/LayerStack.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Prefetch.h"
#include "Strata/Core/Timer.h"
#include "Strata/Core/UUID.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <set>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;

TEST_SUITE("Core")
{
	TEST_CASE("UUID generation is non-zero and unique")
	{
		std::set<uint64_t> generated;
		for (int index = 0; index < 10000; index++)
		{
			UUID uuid;
			CHECK(uuid.IsValid());
			CHECK(generated.insert(static_cast<uint64_t>(uuid)).second);
		}
	}

	TEST_CASE("UUID string round trip")
	{
		const UUID uuid(0x0123456789ABCDEFull);
		CHECK(uuid.ToString() == "0123456789ABCDEF");
		CHECK(UUID::FromString("0123456789ABCDEF").value() == uuid);
		CHECK(UUID::FromString("0x0123456789abcdef").value() == uuid);
		CHECK(UUID::FromString("1").value() == UUID(1));

		CHECK_FALSE(UUID::FromString("").has_value());
		CHECK_FALSE(UUID::FromString("XYZ").has_value());
		CHECK_FALSE(UUID::FromString("0123456789ABCDEF0").has_value());
		CHECK_FALSE(UUID::Null().IsValid());
	}

	TEST_CASE("FNV-1a hashing is stable")
	{
		// Reference values for 64-bit FNV-1a.
		static_assert(Hash::FNV1a("") == 14695981039346656037ull);
		CHECK(Hash::FNV1a("a") == 0xaf63dc4c8601ec8cull);
		CHECK(Hash::FNV1a("foobar") == 0x85944171f73967e8ull);
		CHECK(Hash::Combine(1, 2) != Hash::Combine(2, 1));
	}

	TEST_CASE("CRC-32C matches the standard check values, chains, and agrees with the portable implementation")
	{
		auto bytes = [](std::string_view text) { return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()); };
		// The check value of the Castagnoli polynomial, and RFC 3720's test vectors (32 bytes of zeros, of ones).
		CHECK(Hash::CRC32C(bytes("123456789")) == 0xE3069283u);
		CHECK(Hash::CRC32C({}) == 0u);
		const std::vector<uint8_t> zeros(32, 0x00);
		const std::vector<uint8_t> ones(32, 0xFF);
		CHECK(Hash::CRC32C(zeros) == 0x8A9136AAu);
		CHECK(Hash::CRC32C(ones) == 0x62A8AB43u);

		// Every length and alignment the accelerated path splits differently, against the portable one, and chaining.
		std::vector<uint8_t> data(4096 + 13);
		uint32_t state = 12345;
		for (uint8_t& byte : data)
		{
			state = state * 1664525u + 1013904223u;
			byte = static_cast<uint8_t>(state >> 24);
		}
		for (size_t offset : { size_t(0), size_t(1), size_t(3), size_t(7) })
		{
			for (size_t length : { size_t(0), size_t(1), size_t(7), size_t(8), size_t(9), size_t(63), size_t(64), size_t(1000), size_t(4096) })
			{
				CAPTURE(offset);
				CAPTURE(length);
				const std::span<const uint8_t> range(data.data() + offset, length);
				const uint32_t whole = Hash::CRC32C(range);
				CHECK(whole == Hash::CRC32CPortable(range));
				const size_t split = length / 3;
				CHECK(Hash::CRC32C(range.subspan(split), Hash::CRC32C(range.first(split))) == whole);
				CHECK(Hash::CRC32CPortable(range.subspan(split), Hash::CRC32CPortable(range.first(split))) == whole);
			}
		}
		// A flipped bit changes the checksum.
		const uint32_t before = Hash::CRC32C(data);
		data[100] ^= 0x10;
		CHECK(Hash::CRC32C(data) != before);
	}

	TEST_CASE("LogBuffer keeps the most recent entries with increasing sequence numbers")
	{
		LogBuffer buffer(3);
		buffer.Push(LogLevel::Info, "Test", "one", 0.0);
		buffer.Push(LogLevel::Warn, "Test", "two", 0.0);
		buffer.Push(LogLevel::Error, "Test", "three", 0.0);
		buffer.Push(LogLevel::Info, "Test", "four", 0.0);

		std::vector<LogEntry> entries = buffer.GetEntries();
		REQUIRE(entries.size() == 3);
		CHECK(entries[0].Message == "two");
		CHECK(entries[2].Message == "four");
		CHECK(entries[0].Sequence < entries[1].Sequence);
		CHECK(buffer.GetLatestSequence() == 4);

		std::vector<LogEntry> newer = buffer.GetEntries(entries[1].Sequence);
		REQUIRE(newer.size() == 1);
		CHECK(newer[0].Message == "four");

		CHECK(buffer.GetEntries(0, 1).size() == 1);
	}

	TEST_CASE("Log messages reach the shared buffer")
	{
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		ST_CORE_ERROR("buffer-test {}", 42);
		std::vector<LogEntry> entries = Log::GetBuffer().GetEntries(before);
		REQUIRE(!entries.empty());
		CHECK(entries.back().Message == "buffer-test 42");
		CHECK(entries.back().Level == LogLevel::Error);
		CHECK(entries.back().Logger == "Strata");
	}

	TEST_CASE("FramePacer keeps a loop at or below its frame rate")
	{
		// 100 frames per second: 20 frames take at least 19 periods after the first one.
		FramePacer pacer(100);
		CHECK(pacer.GetMaxFrameRate() == 100);
		const auto start = std::chrono::steady_clock::now();
		for (int frame = 0; frame < 20; frame++)
			pacer.WaitForNextFrame();
		const auto paced = std::chrono::steady_clock::now() - start;
		CHECK(paced >= std::chrono::milliseconds(190));
		CHECK(paced < std::chrono::milliseconds(5000));

		// A frame that runs late lets the next one start at once, without a burst of make-up frames afterwards (which
		// would return at once until the schedule caught up with the ten missed slots).
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		pacer.WaitForNextFrame();
		const auto afterLate = std::chrono::steady_clock::now();
		for (int frame = 0; frame < 5; frame++)
			pacer.WaitForNextFrame();
		CHECK(std::chrono::steady_clock::now() - afterLate >= std::chrono::milliseconds(45));

		// Unlimited: never waits.
		pacer.SetMaxFrameRate(0);
		const auto unlimitedStart = std::chrono::steady_clock::now();
		for (int frame = 0; frame < 1000; frame++)
			pacer.WaitForNextFrame();
		CHECK(std::chrono::steady_clock::now() - unlimitedStart < std::chrono::milliseconds(100));
	}

	TEST_CASE("FramePacer switches between capped and unlimited rates")
	{
		// Unlimited, then 50 frames per second (the editor idling), then unlimited again (activity).
		FramePacer pacer;
		auto measure = [&pacer](int frames)
		{
			const auto start = std::chrono::steady_clock::now();
			for (int frame = 0; frame < frames; frame++)
				pacer.WaitForNextFrame();
			return std::chrono::steady_clock::now() - start;
		};
		CHECK(measure(200) < std::chrono::milliseconds(50));
		pacer.SetMaxFrameRate(50);
		CHECK(pacer.GetMaxFrameRate() == 50);
		// The first capped frame waits a whole period: 5 frames take at least 5 periods.
		CHECK(measure(5) >= std::chrono::milliseconds(95));
		pacer.SetMaxFrameRate(0);
		CHECK(measure(200) < std::chrono::milliseconds(50));

		// Setting the current rate again keeps the schedule: a frame that already waited past its slot is not followed by
		// a full period, as a restarted schedule would do.
		pacer.SetMaxFrameRate(20);
		pacer.WaitForNextFrame();
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		pacer.SetMaxFrameRate(20);
		const auto start = std::chrono::steady_clock::now();
		pacer.WaitForNextFrame();
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(45));
	}

	TEST_CASE("CommandLine parses flags and options")
	{
		CommandLine commandLine(std::vector<std::string> { "app", "--headless", "--project", "Game/Game.stproj", "--frames=120", "--bad=12x" });
		CHECK(commandLine.HasFlag("--headless"));
		CHECK_FALSE(commandLine.HasFlag("--missing"));
		CHECK(commandLine.GetOption("--project").value() == "Game/Game.stproj");
		CHECK(commandLine.GetIntOption("--frames").value() == 120);
		CHECK_FALSE(commandLine.GetIntOption("--bad").has_value());
		CHECK_FALSE(commandLine.GetOption("--missing").has_value());
	}

	TEST_CASE("EventDispatcher routes by type and records handling")
	{
		KeyPressedEvent keyEvent(65, false);
		EventDispatcher dispatcher(keyEvent);

		bool resizeCalled = false;
		CHECK_FALSE(dispatcher.Dispatch<WindowResizeEvent>([&](WindowResizeEvent&) { resizeCalled = true; return true; }));
		CHECK_FALSE(resizeCalled);

		CHECK(dispatcher.Dispatch<KeyPressedEvent>([](KeyPressedEvent& event) { return event.GetKeyCode() == 65; }));
		CHECK(keyEvent.Handled);
		CHECK(keyEvent.IsInCategory(EventCategoryKeyboard));
		CHECK(keyEvent.IsInCategory(EventCategoryInput));
		CHECK_FALSE(keyEvent.IsInCategory(EventCategoryMouse));
	}

	namespace
	{
		struct RecordingLayer : public Layer
		{
			RecordingLayer(const std::string& name, std::vector<std::string>& log)
				: Layer(name), Log(log)
			{
			}

			~RecordingLayer() override { Log.push_back("destroy " + m_DebugName); }
			void OnDetach() override { Log.push_back("detach " + m_DebugName); }

			std::vector<std::string>& Log;
		};
	}

	TEST_CASE("LayerStack keeps overlays above layers and detaches top-down")
	{
		std::vector<std::string> log;
		{
			LayerStack stack;
			stack.PushOverlay(new RecordingLayer("overlay", log));
			stack.PushLayer(new RecordingLayer("a", log));
			stack.PushLayer(new RecordingLayer("b", log));

			std::vector<std::string> order;
			for (Layer* layer : stack)
				order.push_back(layer->GetName());
			CHECK(order == std::vector<std::string> { "a", "b", "overlay" });
		}

		CHECK(log == std::vector<std::string> { "detach overlay", "destroy overlay", "detach b", "destroy b", "detach a", "destroy a" });
	}

	TEST_CASE("Prefetch is a hint that never faults")
	{
		// Script dispatch prefetches objects it may skip, among them module memory it must not read outside a guarded call:
		// a prefetch must leave everything but the caches alone, whatever the address.
		std::vector<int> values(1024, 7);
		for (const int& value : values)
			Prefetch(&value);
		Prefetch(nullptr);
		Prefetch(reinterpret_cast<const void*>(uintptr_t(64))); // In the first page, which is never mapped
		CHECK(std::count(values.begin(), values.end(), 7) == 1024);
	}
}
