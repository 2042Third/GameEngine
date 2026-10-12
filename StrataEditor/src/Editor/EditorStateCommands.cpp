#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/RecentProjects.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>
#include <Strata/Reflection/PropertyJson.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// About four and a half hours at 60 frames per second.
		constexpr int64_t c_MaxWaitFrames = 1'000'000;
		// Frames editor.perf summarizes by default: two seconds at 60 frames per second.
		constexpr int64_t c_DefaultPerfFrames = 120;

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

		nlohmann::json DescribeFrameTimeStats(const FrameTimeStats& stats)
		{
			return { { "avgMs", stats.Average }, { "p50Ms", stats.P50 }, { "p95Ms", stats.P95 }, { "maxMs", stats.Max } };
		}

	}

	namespace CommandUtils
	{

		nlohmann::json DescribeFrameProfile(const FrameProfileSummary& summary, const JobSystemStats& jobs)
		{
			nlohmann::json stages = nlohmann::json::object();
			for (const FrameStageSummary& stage : summary.Stages)
			{
				nlohmann::json entry = DescribeFrameTimeStats(stage.Time);
				entry["inFrameTime"] = stage.InWorkTime;
				stages[stage.Name] = std::move(entry);
			}
			return {
				{ "frames", summary.Frames },
				{ "frame", DescribeFrameTimeStats(summary.Work) },
				{ "framesPerSecond", summary.FramesPerSecond },
				{ "stageSum", DescribeFrameTimeStats(summary.StageSum) },
				{ "untimed", DescribeFrameTimeStats(summary.Untimed) },
				{ "jobs", {
					{ "workerThreads", summary.WorkerThreads },
					{ "ioThreads", summary.IOThreads },
					{ "backgroundThreads", summary.BackgroundThreads },
					{ "workerBusyPercent", summary.WorkerBusyPercent },
					{ "ioBusyPercent", summary.IOBusyPercent },
					{ "backgroundBusyPercent", summary.BackgroundBusyPercent },
					{ "running", jobs.Workers.RunningJobs + jobs.IO.RunningJobs + jobs.Background.RunningJobs },
					{ "queued", jobs.Workers.QueuedJobs + jobs.IO.QueuedJobs + jobs.Background.QueuedJobs } } },
				{ "stages", std::move(stages) } };
		}

	}

	void RegisterEditorStateCommands(EditorCommandRegistry& registry)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Selection
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "selection.get",
			"The selected entities (the last one is the primary selection) and the asset the inspector shows instead of them (inspectedAsset, null "
			"while the entities are inspected).",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				nlohmann::json ids = nlohmann::json::array();
				for (UUID id : context.GetSelection())
					ids.push_back(UUIDToJson(id));
				const AssetHandle inspected = context.GetInspectedAsset();
				return EditorCommandResult::Ok({ { "entities", std::move(ids) },
					{ "inspectedAsset", inspected.IsValid() ? UUIDToJson(inspected) : nlohmann::json(nullptr) } });
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "selection.set",
			"Replaces the selection of entities (an empty array clears it; selecting entities shows them in the inspector) and/or shows an asset in "
			"the asset inspector (asset; null returns to the entities), as clicking it in the Content Browser does. The entities stay selected "
			"while an asset is inspected.",
			ObjectSchema({
				{ "entities", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Entity IDs" } } },
				{ "asset", { { "type", { "string", "null" } }, { "description", "Asset to inspect (handle or path), or null for the entities" } } } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				const auto entities = parameters.find("entities");
				const auto asset = parameters.find("asset");
				if (entities == parameters.end() && asset == parameters.end())
					return EditorCommandResult::InvalidParameters("Missing parameter 'entities' (or 'asset')");
				std::optional<std::vector<UUID>> selection;
				if (entities != parameters.end())
				{
					if (!entities->is_array())
						return EditorCommandResult::InvalidParameters("Parameter 'entities' must be an array of entity IDs");
					selection.emplace();
					for (const nlohmann::json& element : *entities)
					{
						std::optional<UUID> id = UUIDFromJson(element);
						if (!id || !context.GetActiveScene()->GetEntityByUUID(*id))
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'entities': no entity {} in the scene", element.dump()));
						selection->push_back(*id);
					}
				}
				AssetHandle inspected = UUID::Null();
				if (asset != parameters.end() && !asset->is_null())
				{
					CommandArguments arguments(parameters);
					inspected = ResolveAsset(context, arguments, "asset", AssetType::None);
					if (!arguments.IsValid())
						return arguments.Fail();
				}
				if (selection)
					context.SetSelection(std::move(*selection));
				if (asset != parameters.end() && !context.SetInspectedAsset(inspected))
					return EditorCommandResult::Fail("The asset is not available to inspect");
				return EditorCommandResult::Ok();
			}, EditorCommandKind::Session });

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
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Activity
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.activity",
			"What people, agents and scripts did in this editor: the activity journal of every command run (the latest 2,000, within 8 MB), oldest "
			"first after since. Each entry: {id, parent (the command that ran it), time (milliseconds since 1970-01-01 UTC), source (UI, "
			"Automation, CommandScript or Internal), client (an automation client's name), command, mutating, playing (ran while the scene played), "
			"runningCopy (changed the playing scene's running copy, which play.stop discarded), parameters (parametersTruncated when too large to keep), status (Pending, Succeeded, Failed), result (a summary) or error {kind, "
			"message}, warning, durationMs, frames (of a command that took several), entities (that its undo step touched, at most 256) and "
			"entityCount, undoStep}. Page with since = the returned latest while more is true; editor.activity.export turns entries into a "
			"command script.",
			ObjectSchema({
				{ "since", IntegerSchema("Return entries with an id above this (default 0: from the oldest kept)", 0, std::numeric_limits<int64_t>::max()) },
				{ "limit", IntegerSchema("Most entries to return (default 100)", 1, static_cast<int64_t>(CommandJournal::c_MaxEntries)) },
				{ "source", { { "type", "string" }, { "enum", { "UI", "Automation", "CommandScript", "Internal" } },
					{ "description", "Only entries of this source" } } },
				{ "command", StringSchema("Only entries of this command, or of a group with a prefix ending in '.' (\"entity.\")") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t since = arguments.GetInt("since", 0, 0, std::numeric_limits<int64_t>::max());
				const int64_t limit = arguments.GetInt("limit", 100, 1, static_cast<int64_t>(CommandJournal::c_MaxEntries));
				const std::string sourceName = arguments.GetString("source", "");
				const std::string command = arguments.GetString("command", "");
				std::optional<CommandSource> source;
				if (arguments.IsValid() && !sourceName.empty())
				{
					source = CommandSourceFromString(sourceName);
					if (!source)
						arguments.SetError(fmt::format("Parameter 'source' must be UI, Automation, CommandScript or Internal, not '{}'", sourceName));
				}
				if (!arguments.IsValid())
					return arguments.Fail();

				const CommandJournal& journal = context.GetJournal();
				// This call's own entry is still running: it is not part of the answer.
				const uint64_t self = journal.GetRunningId();
				const bool group = !command.empty() && command.back() == '.';
				nlohmann::json entries = nlohmann::json::array();
				uint64_t latest = static_cast<uint64_t>(since);
				bool more = false;
				for (const CommandJournalEntry& entry : journal.GetEntries())
				{
					if (entry.Id <= static_cast<uint64_t>(since) || entry.Id == self)
						continue;
					if (source && entry.Source != *source)
						continue;
					if (!command.empty() && (group ? !entry.Command.starts_with(command) : entry.Command != command))
						continue;
					if (entries.size() == static_cast<size_t>(limit))
					{
						more = true;
						break;
					}
					latest = entry.Id;
					entries.push_back(CommandJournal::Describe(entry));
				}
				return EditorCommandResult::Ok({ { "entries", std::move(entries) }, { "latest", latest }, { "more", more },
					{ "journal", { { "entries", journal.GetEntries().size() }, { "bytes", journal.GetBytes() }, { "latest", journal.GetLatestId() } } } });
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "editor.activity.export",
			"Writes the successful edits after an entry of the activity journal (editor.activity; since 0: from the session's start) as a command script for "
			"StrataEditor --commands, to repeat a session on a copy of the project as it was at that point, e.g. as a regression script: "
			"commands of kind Edit (editor.commands), also those run while playing that outlast play mode (materials, assets, scene.save), but "
			"not edits of a playing scene's running copy (play.stop discarded them: skippedWhilePlaying). Entities and assets the session "
			"created get new IDs every run, so the script binds them where a result gave them (\"bind\": {\"e12.id\": \"/id\"}) and refers to them "
			"where they are used ({\"$ref\": \"e12.id\"}); queries of the project's content whose results later edits took IDs from are part of "
			"the script too (IDs from results that follow the editor's session, such as selection.get and editor.status, stay as they are). "
			"Returns {path, steps, edits, lookups, bindings, skippedWhilePlaying, editsOutsideCommands, unrepeatableEdits, warning?, since, "
			"latest}: changes made outside commands (inspector fields, gizmo drags, the material editor record their undo steps directly) and "
			"the undo steps of commands a script does not repeat (scene.recover) cannot be in the script, and a warning says so when there "
			"were any. Fails when the journal no longer holds every entry after since (it keeps at most 2,000 "
			"commands and 8 MB of them: export from a later entry then), or when the parameters of a command the script needs were too large to keep.",
			ObjectSchema({
				{ "path", StringSchema("The .json file to write: relative to the project directory (needs an open project) or absolute; folders are created, "
					"network and device paths are refused") },
				{ "since", IntegerSchema("Repeat the edits after this journal entry (default 0: from the session's first command, which the journal must "
					"still hold)", 0, std::numeric_limits<int64_t>::max()) },
				{ "overwrite", BoolSchema("Replace an existing file at path (default false)") } },
				{ "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				const int64_t since = arguments.GetInt("since", 0, 0, std::numeric_limits<int64_t>::max());
				const bool overwrite = arguments.GetBool("overwrite", false);
				std::optional<std::filesystem::path> file;
				if (arguments.IsValid())
				{
					std::string error;
					file = ResolveOutputPath(context, path, ".json", overwrite, &error);
					if (!file)
						arguments.SetError(fmt::format("Parameter 'path': {}", error));
				}
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				const std::optional<ReplayScript> script = context.GetJournal().BuildReplayScript(static_cast<uint64_t>(since), &error);
				if (!script)
					return EditorCommandResult::Fail(error);
				if (!FileSystem::CreateDirectories(file->parent_path()) || !FileSystem::WriteText(*file, JsonUtils::Dump(script->Steps, 1, '\t') + "\n"))
					return EditorCommandResult::Fail(fmt::format("Cannot write the script to '{}'", FileSystem::ToUTF8(*file)));
				nlohmann::json value = {
					{ "path", FileSystem::ToUTF8(*file) },
					{ "steps", script->Steps.size() },
					{ "edits", script->Edits },
					{ "lookups", script->Lookups },
					{ "bindings", script->Bindings },
					{ "skippedWhilePlaying", script->SkippedWhilePlaying },
					{ "editsOutsideCommands", script->EditsOutsideCommands },
					{ "unrepeatableEdits", script->UnrepeatableEdits },
					{ "since", script->Since },
					{ "latest", script->Latest } };
				std::string missing;
				if (script->EditsOutsideCommands > 0)
					missing = fmt::format("{} edit(s) were made outside commands (inspector, gizmos, material editor)", script->EditsOutsideCommands);
				if (script->UnrepeatableEdits > 0)
				{
					missing += fmt::format("{}{} edit(s) came from commands a script does not repeat (scene.recover)", missing.empty() ? "" : " and ",
						script->UnrepeatableEdits);
				}
				if (!missing.empty())
					value["warning"] = fmt::format("Since #{}, {}: the script does not repeat them", script->Since, missing);
				return EditorCommandResult::Ok(std::move(value));
			}, EditorCommandKind::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Recovery
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "scene.recoveryStatus",
			"The recovery data of the open project (.strata/Recovery): unsaved scene changes an editor left behind when it ended without closing "
			"the scene (a crash), or that were discarded on purpose (force, Don't Save) and kept anyway. Returns entries, newest first, each {id, "
			"scene (null for a scene never saved), sceneName, scenePath, time (milliseconds since 1970-01-01 UTC), discarded, entityCount, complete "
			"(a snapshot of every entity), applicable, problem (why it cannot be applied, e.g. the scene was saved since)}, and the journal of the "
			"edited scene {active, file, writes, lastWrite, pending (changes not written yet: they are written about 2 s after edits stop), busy, "
			"skipped (why its changes are not journaled)}. Reads the directory again, after the journal writes already asked for. Apply or "
			"discard an entry with scene.recover. Fails without a project.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (!context.GetRecovery().IsActive())
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");
				context.GetRecovery().Scan();
				return EditorCommandResult::Defer([](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					const SceneRecovery& recovery = context.GetRecovery();
					if (recovery.IsScanning())
						return std::nullopt;
					nlohmann::json entries = nlohmann::json::array();
					for (const RecoveryEntry& entry : recovery.GetEntries())
						entries.push_back(SceneRecovery::DescribeEntry(entry));
					nlohmann::json result = recovery.DescribeStatus();
					result["entries"] = std::move(entries);
					return EditorCommandResult::Ok(std::move(result));
				});
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "scene.recover",
			"Applies or discards recovery data (scene.recoveryStatus). apply: opens the scene the changes were made to (a scene never saved: a "
			"new scene of its name) and turns it into what the edits left, as one undo step back to the saved scene; the scene stays modified "
			"(save it to keep the changes). It refuses when the scene file changed since the changes were made (applicable false), while "
			"playing, and while the edited scene has unsaved changes it would discard unless force is true. discard: moves the data to "
			".strata/Recovery/Discarded, where it is not offered again. Returns {recovered | discarded: id, ...}; apply also scene, sceneName, "
			"entities (how many it restored or removed) and discardedChanges.",
			ObjectSchema({
				{ "scene", StringSchema("The id of an entry of scene.recoveryStatus, or a scene asset (handle or path) to use its newest applicable entry") },
				{ "action", { { "type", "string" }, { "enum", { "apply", "discard" } }, { "description", "\"apply\" the changes or \"discard\" them" } } },
				{ "force", ForceSchema() } },
				{ "scene", "action" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string reference = arguments.GetString("scene");
				const std::string action = arguments.GetString("action");
				const bool force = arguments.GetBool("force", false);
				if (arguments.IsValid() && action != "apply" && action != "discard")
					arguments.SetError("Parameter 'action' must be \"apply\" or \"discard\"");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!context.GetRecovery().IsActive())
					return EditorCommandResult::Fail("No project is open (project.open or project.create)");

				// The entries come from the scan the project's opening (or scene.recoveryStatus) started: wait for it first.
				struct Progress
				{
					Ref<SceneRecovery::Load> Load;
				};
				auto progress = std::make_shared<Progress>();
				return EditorCommandResult::Defer([reference, action, force, progress](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					SceneRecovery& recovery = context.GetRecovery();
					if (progress->Load)
					{
						if (!progress->Load->IsDone())
							return std::nullopt;
						if (!progress->Load->Error.empty())
							return EditorCommandResult::Fail(progress->Load->Error);
						const RecoveryData& data = *progress->Load->Data;
						const bool replaces = !data.Entry.Scene.IsValid() || context.GetSceneHandle() != data.Entry.Scene || context.IsSceneModified();
						const bool discarded = replaces && context.IsSceneModified();
						size_t applied = 0;
						std::string error;
						if (!context.ApplyRecovery(data, force, &applied, &error))
							return EditorCommandResult::Fail(error);
						return EditorCommandResult::Ok({
							{ "recovered", data.Entry.Id },
							{ "scene", data.Entry.Scene.IsValid() ? UUIDToJson(data.Entry.Scene) : nlohmann::json(nullptr) },
							{ "sceneName", data.Entry.SceneName },
							{ "entities", applied },
							{ "discardedChanges", discarded } });
					}
					if (recovery.IsScanning())
						return std::nullopt;

					// An entry's id, or the newest entry of a scene (applicable ones first).
					const RecoveryEntry* entry = recovery.FindEntry(reference);
					if (!entry)
					{
						if (const std::optional<FoundAsset> scene = FindAsset(reference); scene && scene->Type == AssetType::Scene)
						{
							for (const RecoveryEntry& candidate : recovery.GetEntries())
							{
								if (candidate.Scene == scene->Handle && (!entry || (candidate.Applicable && !entry->Applicable)))
									entry = &candidate;
							}
						}
					}
					if (!entry)
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'scene': no recovery data for '{}' (scene.recoveryStatus lists what there is)", reference));
					if (action == "discard")
					{
						const std::string id = entry->Id;
						std::string error;
						if (!recovery.Discard(id, &error))
							return EditorCommandResult::Fail(error);
						return EditorCommandResult::Ok({ { "discarded", id } });
					}
					if (!entry->Applicable)
						return EditorCommandResult::Fail(fmt::format("The recovery data '{}' cannot be applied: {}. Discard it with action \"discard\"", entry->Id, entry->Problem));
					const bool replaces = !entry->Scene.IsValid() || context.GetSceneHandle() != entry->Scene || context.IsSceneModified();
					if (replaces && !force)
					{
						if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, false, fmt::format("recovering '{}'", entry->SceneName)))
							return std::move(*refused);
					}
					if (context.IsPlaying())
						return EditorCommandResult::Fail("Stop play mode first (play.stop): recovering replaces the edited scene");
					progress->Load = recovery.Read(entry->Id, context.GetSceneFile(entry->Scene).first);
					return std::nullopt;
				});
			}, EditorCommandKind::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Editor
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.status",
			"Overview of the editor: engine version and commit, open project, scene (name, asset, unsaved changes, entity count), play state, "
			"selection, undo history, and sections such as automation (port, clients, pending requests and commands) and assets (memory and "
			"budgets per pool, streaming queue, uploads; see asset.stats). Start here to orient yourself.",
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
						{ "primary", primary ? UUIDToJson(primary.GetUUID()) : nlohmann::json(nullptr) },
						{ "inspectedAsset", context.GetInspectedAsset().IsValid() ? UUIDToJson(context.GetInspectedAsset()) : nlohmann::json(nullptr) } } },
					{ "undo", {
						{ "position", undo.GetPosition() },
						{ "count", undo.GetHistory().size() },
						{ "undo", undo.GetUndoName() },
						{ "redo", undo.GetRedoName() } } } };
				// Sections of other editor parts; they never replace the built-in ones.
				for (const auto& [section, provider] : context.GetStatusProviders())
					status.emplace(section, provider());
				return EditorCommandResult::Ok(std::move(status));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

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
			}, EditorCommandKind::Query, EditorCommandResults::Session });

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
			}, EditorCommandKind::Session });

		registry.Register({ "editor.quit",
			"Closes the editor after answering (pending commands are cancelled). Fails while the scene has unsaved changes unless force is "
			"true; save first with scene.save or scene.saveAs.",
			ObjectSchema({ { "force", ForceSchema() } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, "quitting"))
					return std::move(*refused);
				const bool modified = context.IsSceneModified();
				context.RequestQuit();
				return EditorCommandResult::Ok({ { "quitting", true }, { "discardedChanges", modified } });
			}, EditorCommandKind::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Frames
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.wait",
			"Returns after the given number of frames. Use it to let a playing scene run, assets load or the viewport render before the next "
			"command. Returns {frames} and, when the editor measures its frames (not when a test drives it without its EditorLayer), "
			"frameTimes: the CPU time of the frames that ran entirely while waiting (all but the first), in milliseconds {count, medianMs, "
			"meanMs, p95Ms, maxMs}, without waiting for the GPU and the display, so command scripts can check frame budgets with expect.",
			ObjectSchema({ { "frames", IntegerSchema("Frames to wait (default 1)", 1, c_MaxWaitFrames) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t frames = arguments.GetInt("frames", 1, 1, c_MaxWaitFrames);
				if (!arguments.IsValid())
					return arguments.Fail();
				// Polled once per frame from the frame after this one, so it finishes when `frames` frames have passed. Each poll
				// sees the time of the frame before it. The frame this command runs in also runs whatever came before it (in a
				// command script, e.g. a scene load), so the measured frames start with the next one.
				const uint64_t firstMeasured = context.GetRecordedFrameCount() + 2;
				return EditorCommandResult::Defer([frames, remaining = frames, firstMeasured, lastRecorded = uint64_t(0), frameTimes = std::vector<double>()](
					EditorContext& context) mutable -> std::optional<EditorCommandResult>
				{
					const uint64_t recorded = context.GetRecordedFrameCount();
					if (recorded >= firstMeasured && recorded != lastRecorded)
					{
						frameTimes.push_back(context.GetLastFrameTime());
						lastRecorded = recorded;
					}
					if (--remaining > 0)
						return std::nullopt;
					nlohmann::json result = { { "frames", frames } };
					if (!frameTimes.empty())
						result["frameTimes"] = DescribeFrameTimes(std::move(frameTimes));
					return EditorCommandResult::Ok(std::move(result));
				});
			}, EditorCommandKind::Query });

		registry.Register({ "editor.perf",
			"Where the editor's frames go: statistics of the latest frames the frame profiler recorded (at most 600). Returns frames (how many "
			"were summarized), frame (the CPU time of a frame without the waits for the GPU and the display, like the status bar's frame time), "
			"framesPerSecond, stages (per stage of the main thread: Input, Assets.Update, Assets.Finalize, Scene.Scripts, Scene.Physics, "
			"Scene.Audio, Scene.Transforms, Render.Extract, Render.Submit, Editor.UI and Present, plus stages modules registered; each its self "
			"time, nested stages excluded, with inFrameTime false for Present, which comes after the frame time is measured), stageSum (the "
			"stages inside the frame time added up per frame), untimed (the rest of each frame, work no stage accounts for) and jobs (worker, "
			"I/O and background threads (imports, exports), how busy they were in percent, running and queued jobs now). Times are milliseconds as avgMs, p50Ms and p95Ms "
			"(nearest rank) and maxMs.",
			ObjectSchema({ { "frames", IntegerSchema("How many of the latest frames to summarize (default 120)", 1, FrameProfiler::c_DefaultCapacity) } }),
			[](EditorContext&, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t frames = arguments.GetInt("frames", c_DefaultPerfFrames, 1, FrameProfiler::c_DefaultCapacity);
				if (!arguments.IsValid())
					return arguments.Fail();
				const FrameProfileSummary summary = FrameProfiler::Get().Summarize(static_cast<uint32_t>(frames));
				return EditorCommandResult::Ok(DescribeFrameProfile(summary, JobSystem::GetStats()));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

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
			}, EditorCommandKind::Session });

		registry.Register({ "play.simulate", "Runs a copy of the scene with physics only (no scripts or audio).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.Simulate(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

		registry.Register({ "play.stop", "Stops play or simulate mode and returns to the edited scene.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				context.Stop();
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Session });

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
			}, EditorCommandKind::Session });

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
			}, EditorCommandKind::Session });

		registry.Register({ "play.state", "Whether the scene is being edited, played or simulated, and whether it is paused.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribePlayState(context));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

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
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}

}
