#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/RecentProjects.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>
#include <Strata/Reflection/PropertyJson.h>

#include <limits>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// About four and a half hours at 60 frames per second.
		constexpr int64_t c_MaxWaitFrames = 1'000'000;

		nlohmann::json DescribePlayState(const EditorContext& context)
		{
			return { { "state", SceneStateToString(context.GetSceneState()) }, { "paused", context.IsPaused() } };
		}

		nlohmann::json DescribeHistory(EditorContext& context)
		{
			const UndoStack& undo = context.GetUndoStack();
			return {
				{ "history", undo.GetHistory() },
				{ "position", undo.GetPosition() },
				{ "undo", undo.GetUndoName() },
				{ "redo", undo.GetRedoName() } };
		}

	}

	void RegisterEditorStateCommands(EditorCommandRegistry& registry)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Selection
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "selection.get", "The selected entities; the last one is the primary selection.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				nlohmann::json ids = nlohmann::json::array();
				for (UUID id : context.GetSelection())
					ids.push_back(UUIDToJson(id));
				return EditorCommandResult::Ok({ { "entities", std::move(ids) } });
			} });

		registry.Register({ "selection.set", "Replaces the selection (an empty array clears it).",
			ObjectSchema({ { "entities", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Entity IDs" } } } }, { "entities" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				auto entities = parameters.find("entities");
				if (entities == parameters.end() || !entities->is_array())
					return EditorCommandResult::InvalidParameters("Parameter 'entities' must be an array of entity IDs");
				std::vector<UUID> selection;
				for (const nlohmann::json& element : *entities)
				{
					std::optional<UUID> id = UUIDFromJson(element);
					if (!id || !context.GetActiveScene()->GetEntityByUUID(*id))
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'entities': no entity {} in the scene", element.dump()));
					selection.push_back(*id);
				}
				context.SetSelection(std::move(selection));
				return EditorCommandResult::Ok();
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Undo
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "edit.undo", "Undoes the latest edit of the scene (not available while playing).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (context.IsPlaying())
					return EditorCommandResult::Fail("Undo is not available while the scene is running");
				if (!context.Undo())
					return EditorCommandResult::Fail("Nothing to undo");
				return EditorCommandResult::Ok(DescribeHistory(context));
			} });

		registry.Register({ "edit.redo", "Redoes the latest undone edit (not available while playing).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (context.IsPlaying())
					return EditorCommandResult::Fail("Redo is not available while the scene is running");
				if (!context.Redo())
					return EditorCommandResult::Fail("Nothing to redo");
				return EditorCommandResult::Ok(DescribeHistory(context));
			} });

		registry.Register({ "edit.history", "Names of the recorded edits, oldest first, and how many are applied.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribeHistory(context));
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Editor
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.status",
			"Overview of the editor: engine version and commit, open project, scene (name, asset, unsaved changes, entity count), play state, selection, "
			"undo history, and sections such as automation (port, clients, pending requests and commands). Start here to orient yourself.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				Entity primary = context.GetPrimarySelection();
				const UndoStack& undo = context.GetUndoStack();
				nlohmann::json status = {
					{ "engineVersion", c_EngineVersion },
					{ "engineCommit", c_EngineCommit },
					{ "platform", std::string(Platform::GetName()) },
					{ "project", DescribeProject(context) },
					{ "scene", {
						{ "name", context.GetEditScene()->GetName() },
						{ "scene", context.GetSceneHandle().IsValid() ? UUIDToJson(context.GetSceneHandle()) : nlohmann::json(nullptr) },
						{ "modified", context.IsSceneModified() },
						{ "entityCount", context.GetActiveScene()->GetEntityCount() } } },
					{ "play", DescribePlayState(context) },
					{ "selection", {
						{ "count", context.GetSelection().size() },
						{ "primary", primary ? UUIDToJson(primary.GetUUID()) : nlohmann::json(nullptr) } } },
					{ "undo", {
						{ "position", undo.GetPosition() },
						{ "count", undo.GetHistory().size() },
						{ "undo", undo.GetUndoName() },
						{ "redo", undo.GetRedoName() } } } };
				// Sections of other editor parts; they never replace the built-in ones.
				for (const auto& [section, provider] : context.GetStatusProviders())
					status.emplace(section, provider());
				return EditorCommandResult::Ok(std::move(status));
			} });

		registry.Register({ "editor.recentProjects",
			"The projects opened most recently, most recent first (at most 12, projects that no longer exist left out): name, path of the project "
			"file (for project.open), lastOpened (seconds since 1970-01-01 UTC) and the engine version that opened it. The list is shared with the "
			"editor's launcher.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				nlohmann::json projects = nlohmann::json::array();
				for (const RecentProject& project : context.GetRecentProjects().GetProjects())
				{
					projects.push_back({
						{ "name", project.Name },
						{ "path", FileSystem::ToUTF8(project.Path) },
						{ "lastOpened", project.LastOpened },
						{ "engineVersion", project.EngineVersion } });
				}
				return EditorCommandResult::Ok({ { "projects", std::move(projects) } });
			} });

		registry.Register({ "editor.removeRecentProject",
			"Takes a project off the recent projects (editor.recentProjects); its files stay where they are.",
			ObjectSchema({ { "path", StringSchema("The project file as editor.recentProjects lists it") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.GetRecentProjects().Remove(FileSystem::FromUTF8(path)))
					return EditorCommandResult::InvalidParameters(fmt::format("'{}' is not a recent project (editor.recentProjects lists them)", path));
				return EditorCommandResult::Ok();
			} });

		registry.Register({ "editor.quit",
			"Closes the editor after answering (pending commands are cancelled). Fails while the scene has unsaved changes unless force is "
			"true; save first with scene.save or scene.saveAs.",
			ObjectSchema({ { "force", BoolSchema("Quit even if the scene has unsaved changes, discarding them (default false)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();

				const bool modified = context.IsSceneModified();
				if (modified && !force)
				{
					return EditorCommandResult::Fail(fmt::format("The scene '{}' has unsaved changes: save them (scene.save, or scene.saveAs for a new "
						"scene) or quit with force: true to discard them", context.GetEditScene()->GetName()));
				}
				context.RequestQuit();
				return EditorCommandResult::Ok({ { "quitting", true }, { "discardedChanges", modified } });
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Frames
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.wait",
			"Returns after the given number of frames. Use it to let a playing scene run, assets load or the viewport render before the next command.",
			ObjectSchema({ { "frames", IntegerSchema("Frames to wait (default 1)", 1, c_MaxWaitFrames) } }),
			[](EditorContext&, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t frames = arguments.GetInt("frames", 1, 1, c_MaxWaitFrames);
				if (!arguments.IsValid())
					return arguments.Fail();
				// Polled once per frame from the frame after this one, so it finishes when `frames` frames have passed.
				return EditorCommandResult::Defer([frames, remaining = frames](EditorContext&) mutable -> std::optional<EditorCommandResult>
				{
					if (--remaining > 0)
						return std::nullopt;
					return EditorCommandResult::Ok({ { "frames", frames } });
				});
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Play mode
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "play.start", "Runs a copy of the scene with scripts, physics and audio. Changes made while playing are discarded by play.stop.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.Play(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok(DescribePlayState(context));
			} });

		registry.Register({ "play.simulate", "Runs a copy of the scene with physics only (no scripts or audio).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.Simulate(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok(DescribePlayState(context));
			} });

		registry.Register({ "play.stop", "Stops play or simulate mode and returns to the edited scene.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				context.Stop();
				return EditorCommandResult::Ok(DescribePlayState(context));
			} });

		registry.Register({ "play.pause", "Pauses or resumes the running scene.",
			ObjectSchema({ { "paused", BoolSchema("Whether the simulation is paused") } }, { "paused" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				if (!arguments.Has("paused"))
					arguments.SetError("Missing parameter 'paused'");
				const bool paused = arguments.GetBool("paused", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPlaying())
					return EditorCommandResult::Fail("The scene is not running");
				context.SetPaused(paused);
				return EditorCommandResult::Ok(DescribePlayState(context));
			} });

		registry.Register({ "play.step", "While paused, advances the simulation by a number of fixed steps over the next frames.",
			ObjectSchema({ { "frames", IntegerSchema("Fixed steps (default 1)", 1, 10000) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t frames = arguments.GetInt("frames", 1, 1, 10000);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.IsPaused())
					return EditorCommandResult::Fail("Stepping needs a paused running scene (play.pause)");
				context.Step(static_cast<uint32_t>(frames));
				return EditorCommandResult::Ok(DescribePlayState(context));
			} });

		registry.Register({ "play.state", "Whether the scene is being edited, played or simulated, and whether it is paused.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribePlayState(context));
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Log
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "log.read", "Log messages (engine, editor and scripts) after a sequence number, oldest first. Page with after = the returned latest while more is true.",
			ObjectSchema({
				{ "after", IntegerSchema("Return messages with a sequence number above this (default 0)", 0, std::numeric_limits<int64_t>::max()) },
				{ "maxCount", IntegerSchema("Maximum number of messages (default 200)", 1, 10000) },
				{ "minLevel", { { "type", "string" }, { "enum", { "Trace", "Info", "Warn", "Error", "Critical" } }, { "description", "Lowest level returned (default Info)" } } } }),
			[](EditorContext&, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t after = arguments.GetInt("after", 0, 0, std::numeric_limits<int64_t>::max());
				const int64_t maxCount = arguments.GetInt("maxCount", 200, 1, 10000);
				const std::string levelName = arguments.GetString("minLevel", "Info");
				LogLevel minLevel = LogLevel::Info;
				bool levelFound = false;
				for (LogLevel level : { LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical })
				{
					if (levelName == LogLevelToString(level))
					{
						minLevel = level;
						levelFound = true;
					}
				}
				if (!levelFound)
					arguments.SetError(fmt::format("Unknown log level '{}'", levelName));
				if (!arguments.IsValid())
					return arguments.Fail();

				// The oldest messages after `after` come first, so paging with after = latest never skips any.
				nlohmann::json messages = nlohmann::json::array();
				uint64_t latest = static_cast<uint64_t>(after);
				bool more = false;
				for (const LogEntry& entry : Log::GetBuffer().GetEntries(static_cast<uint64_t>(after)))
				{
					if (entry.Level < minLevel)
					{
						latest = entry.Sequence;
						continue;
					}
					if (messages.size() == static_cast<size_t>(maxCount))
					{
						more = true;
						break;
					}
					latest = entry.Sequence;
					messages.push_back({
						{ "sequence", entry.Sequence },
						{ "level", LogLevelToString(entry.Level) },
						{ "logger", entry.Logger },
						{ "message", entry.Message },
						{ "time", entry.Timestamp } });
				}
				return EditorCommandResult::Ok({ { "messages", std::move(messages) }, { "latest", latest }, { "more", more } });
			} });
	}

}
