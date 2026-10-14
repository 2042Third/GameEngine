#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <string>

using namespace Strata;

namespace
{

	struct UICommandHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		nlohmann::json SceneSnapshot() const
		{
			return SceneSerializer::Serialize(*Context.GetEditScene())["Scene"]["Entities"];
		}
	};

}

TEST_SUITE("Editor.UICommands")
{
	TEST_CASE("Adding a UI component adds the rect it requires in the same undo step")
	{
		UICommandHarness harness;
		const std::string id = harness.Run("entity.create", { { "name", "Badge" } })["id"].get<std::string>();
		const nlohmann::json before = harness.SceneSnapshot();

		harness.Run("component.add", { { "entity", id }, { "component", "UIImage" }, { "values", { { "CornerRadius", 4.0 } } } });
		const nlohmann::json badge = harness.Run("entity.get", { { "entity", id } });
		CHECK(badge["components"].contains("UIRect"));
		CHECK(badge["components"]["UIImage"]["CornerRadius"] == 4.0);
		const nlohmann::json added = harness.SceneSnapshot();

		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == before);
		harness.Run("edit.redo");
		CHECK(harness.SceneSnapshot() == added);

		// entity.create with a UI component adds the rect too.
		const std::string label = harness.Run("entity.create", { { "name", "Label" }, { "components", { { "UIText", { { "Text", "Hi" } } } } } })["id"].get<std::string>();
		CHECK(harness.Run("entity.get", { { "entity", label } })["components"].contains("UIRect"));
	}
}
