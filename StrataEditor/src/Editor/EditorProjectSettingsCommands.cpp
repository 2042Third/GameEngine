#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/SettingsFileWriter.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Project/Project.h>

#include <memory>
#include <optional>

namespace Strata
{

	using namespace CommandUtils;

	void RegisterProjectSettingsCommands(EditorCommandRegistry& registry)
	{
		registry.Register(MayDefer({ "project.save",
			"Writes the project file now and answers once it is written: the project's settings are written in the background after every change "
			"anyway, so this is for after a write failed (editor.status' project fileState \"unwritten\", fileError says why) once its cause is "
			"fixed, and for waiting until the file holds the latest change. Fails with the reason when the write fails. Returns projectFile.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open");
				context.SaveProjectFile();
				const std::weak_ptr<Project> project = context.GetProject();
				return EditorCommandResult::Defer([project](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					if (project.lock() != context.GetProject())
						return EditorCommandResult::Fail("The project was closed before its file was written", EditorCommandError::Cancelled);
					// Writes coalesce: once none is in flight, the last one held this command's state or a newer one.
					const SettingsFileState state = context.GetProjectFileState();
					if (state == SettingsFileState::Writing)
						return std::nullopt;
					const std::string file = FileSystem::ToUTF8(context.GetProject()->GetProjectFile());
					if (state == SettingsFileState::Unwritten)
						return EditorCommandResult::Fail(fmt::format("Writing '{}' failed: {}", file, context.GetProjectFileError()));
					return EditorCommandResult::Ok({ { "projectFile", file } });
				});
			} }));
	}

}
