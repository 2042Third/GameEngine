#include <doctest/doctest.h>

#include "Reflection/ComponentSchema.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "TestHelpers.h"

#include <map>
#include <optional>
#include <string>

using namespace Strata;

// The snapshot holds the components of every module: a build without some registers fewer.
#if ST_TESTS_HAVE_EVERY_MODULE
TEST_SUITE("Reflection.ComponentSchema")
{
	TEST_CASE("Every component of the snapshot keeps its name, metadata and properties, in the same order")
	{
		// StrataTests/Architecture/ComponentSchema.json was written from the registry before the components moved into
		// their modules (--strata-test-helper=write-component-schema): the move changes where they are declared and
		// registered, never what files, the Inspector, Add Component and component.list see. Components it does not list
		// (added since) are allowed.
		const std::optional<std::string> text = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataTests" / "Architecture" / "ComponentSchema.json");
		REQUIRE(text);
		const std::optional<nlohmann::json> snapshot = JsonUtils::Parse(*text);
		REQUIRE(snapshot);
		const nlohmann::json& expected = (*snapshot)["Components"];
		REQUIRE(expected.is_array());
		REQUIRE(expected.size() > 20);

		const nlohmann::json live = Tests::DescribeComponentSchema()["Components"];
		std::map<std::string, size_t> liveIndex;
		for (size_t index = 0; index < live.size(); index++)
			liveIndex.emplace(live[index]["Name"].get<std::string>(), index);

		std::optional<size_t> previous;
		for (const nlohmann::json& component : expected)
		{
			const std::string name = component["Name"].get<std::string>();
			CAPTURE(name);
			const auto found = liveIndex.find(name);
			REQUIRE_MESSAGE(found != liveIndex.end(), "the component is no longer registered");
			INFO("Registered now: ", JsonUtils::Dump(live[found->second], 1, '\t'));
			INFO("In the snapshot: ", JsonUtils::Dump(component, 1, '\t'));
			CHECK(live[found->second] == component);
			if (previous)
				CHECK(found->second > *previous);
			previous = found->second;
		}
	}
}
#endif
