#include <doctest/doctest.h>

#include "Strata/Core/CommandLine.h"
#include "Strata/Core/Hash.h"
#include "Strata/Core/Layer.h"
#include "Strata/Core/LayerStack.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/UUID.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"

#include <set>
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
}
