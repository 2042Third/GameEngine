#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <nlohmann/json.hpp>

#include <string>

namespace Strata
{

	using namespace CommandUtils;

	void RegisterGraphicsCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "graphics.resetPlayerOptions",
			"Deletes the players' graphics options of the project's game: the file <project>/.strata/Saves/GraphicsOptions.json that a game's "
			"options menu wrote with Graphics::Save while the game played here, and that every play session starts with, so that the "
			"project's Graphics Quality (the settings section Renderer.Quality, project.setSettings) applies in play again (while the file sets "
			"the preset, changes of the project's preset do not show in play; the Graphics Quality page says so). While playing, the running "
			"game also drops the players' choices from its next frame. Not undoable, like the game's saves. Returns reset: the options the "
			"players' choices had set.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&) -> EditorCommandResult
			{
				if (!context.HasProject())
				{
					return EditorCommandResult::Fail("No project is open: the players' graphics options belong to a project's game (project.open or "
						"project.create first)");
				}
				// The options' names, sorted (a JSON object's keys are).
				nlohmann::json reset = nlohmann::json::array();
				for (const auto& [option, value] : context.GetPlayerGraphicsOptions().items())
					reset.push_back(option);
				std::string error;
				if (!context.ResetPlayerGraphicsOptions(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "reset", std::move(reset) } });
			}, EditorCommandKind::Session });
	}

}
