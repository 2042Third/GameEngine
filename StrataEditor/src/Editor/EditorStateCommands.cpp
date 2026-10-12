#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/RecentProjects.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>
#include <Strata/Reflection/PropertyJson.h>

#include <algorithm>
#include <limits>
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

		// The scene scene.open (or project.open, for the start scene) is opening, or null.
		nlohmann::json DescribeSceneLoad(const EditorContext& context)
		{
			if (!context.IsSceneLoading())
				return nullptr;
			const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
			return { { "scene", UUIDToJson(status.Scene) }, { "startScene", status.StartScene }, { "state", SceneLoadStateToString(status.State) },
				{ "progress", status.Progress } };
		}

		// The editor process's memory as the system accounts it (Platform::GetProcessMemory), in MiB; null where unknown.
		nlohmann::json DescribeProcessMemory()
		{
			const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory();
			if (!memory)
				return nullptr;
			const auto toMB = [](uint64_t bytes) { return static_cast<double>(bytes) / static_cast<double>(1ull << 20); };
			return {
				{ "privateMB", toMB(memory->PrivateBytes) },
				{ "workingSetMB", toMB(memory->WorkingSet) },
				{ "peakPrivateMB", toMB(memory->PeakPrivateBytes) },
				{ "peakWorkingSetMB", toMB(memory->PeakWorkingSet) } };
		}

		nlohmann::json DescribeFrameTimeStats(const FrameTimeStats& stats)
		{
			return { { "avgMs", stats.Average }, { "p50Ms", stats.P50 }, { "p95Ms", stats.P95 }, { "maxMs", stats.Max } };
		}

		// Where play.start is while the scripts build first.
		struct PlayBuildState
		{
			uint64_t WaitingFor = 0;   // The build that has to finish before anything else happens (0: none)
			uint64_t OwnBuild = 0;     // The build play.start started itself (it starts at most one)
			std::string Reason;        // Why it built
		};

		// Plays once the build play.start waited for finished: a failed build (or a module that cannot load) fails play.start
		// with its first diagnostics and asks the UI for the Problems panel; a cancelled one only ends play.start (Cancelled).
		std::optional<EditorCommandResult> FinishPlayBuild(EditorContext& context, PlayBuildState& state)
		{
			const ScriptBuildResult& result = context.GetScriptBuilder().GetLastResult();
			const ScriptBuildLoad& load = context.GetLastScriptBuildLoad();
			if (context.GetPlayBuildID() == state.OwnBuild)
				context.ClearPlayBuild();
			if (result.ID != state.WaitingFor)
				return EditorCommandResult::Fail(fmt::format("The result of script build {} is no longer available (see script.status)", state.WaitingFor));
			if (result.Cancelled)
				return EditorCommandResult::Fail("The script build play.start waited for was cancelled, so the game does not play", EditorCommandError::Cancelled);
			if (!result.Success)
			{
				context.RequestProblemsPanel();
				return EditorCommandResult::Fail(fmt::format("The scripts must be built before playing, and the build failed: {}", result.Error));
			}
			if (load.BuildID == result.ID && !load.Loaded)
			{
				context.RequestProblemsPanel();
				return EditorCommandResult::Fail(fmt::format("The scripts were built, but their module could not be loaded: {}", load.Error));
			}

			// A build someone else started may have left the scripts in need of another (sources changed meanwhile): play.start
			// builds once itself.
			if (state.OwnBuild == 0)
			{
				if (std::optional<std::string> reason = context.GetPlayBuildReason())
				{
					std::string error;
					if (!context.StartPlayBuild(&error))
						return EditorCommandResult::Fail(error);
					state.Reason = std::move(*reason);
					state.OwnBuild = state.WaitingFor = context.GetPlayBuildID();
					ST_INFO("Building the scripts before playing: {}", state.Reason);
					return std::nullopt;
				}
			}

			std::string error;
			if (!context.Play(&error))
				return EditorCommandResult::Fail(error);
			nlohmann::json value = DescribePlayState(context);
			if (state.OwnBuild != 0)
				value["built"] = { { "build", state.OwnBuild }, { "seconds", result.Seconds }, { "reason", state.Reason } };
			return EditorCommandResult::Ok(std::move(value));
		}

		// play.start when the scripts need a build (or one runs): builds them (or waits for the running build), then plays.
		EditorCommandResult StartPlayAfterBuild(EditorContext& context)
		{
			Ref<PlayBuildState> state = CreateRef<PlayBuildState>();
			const ScriptBuilder& builder = context.GetScriptBuilder();
			if (builder.IsRunning())
			{
				state->WaitingFor = builder.GetCurrentID();
			}
			else
			{
				state->Reason = context.GetPlayBuildReason().value_or(std::string());
				std::string error;
				if (!context.StartPlayBuild(&error))
					return EditorCommandResult::Fail(fmt::format("The scripts must be built before playing ({}), and the build could not start: {}",
						state->Reason, error));
				state->OwnBuild = state->WaitingFor = context.GetPlayBuildID();
				ST_INFO("Building the scripts before playing: {}", state->Reason);
			}
			return EditorCommandResult::Defer([state](EditorContext& pollContext) -> std::optional<EditorCommandResult>
			{
				const ScriptBuilder& pollBuilder = pollContext.GetScriptBuilder();
				if (pollBuilder.IsRunning() && pollBuilder.GetCurrentID() == state->WaitingFor)
					return std::nullopt;
				return FinishPlayBuild(pollContext, *state);
			});
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
			} });

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
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Undo
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "edit.undo", "Undoes the latest edit of the scene or of the project's files (not available while playing or while a "
			"scene is being opened). Fails, changing nothing, when there is nothing to undo or the step cannot be undone now (e.g. another file "
			"is where a deleted asset would come back), saying why; the step stays in the history. Returns the history like edit.history.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.Undo(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok(DescribeHistory(context));
			} });

		registry.Register({ "edit.redo", "Redoes the latest undone edit (not available while playing or while a scene is being opened). Fails "
			"when there is nothing to redo or the step no longer fits the project (e.g. a deleted asset's path now holds another one), saying "
			"why; such a step and the ones after it leave the history. Returns the history like edit.history.", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				if (!context.Redo(&error))
					return EditorCommandResult::Fail(error);
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
			"Overview of the editor: engine version and commit, open project, scene (name, asset, unsaved changes, entity count), play state, "
			"selection, undo history, sceneLoad (the scene scene.open or project.open is opening: {scene, startScene, state, progress}, null when "
			"none), the process's memory (privateMB, workingSetMB and their peaks), and sections such as automation (port, clients, pending "
			"requests and commands) and assets (memory and budgets per pool, streaming queue, uploads; see asset.stats). Start here to orient "
			"yourself.",
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
						{ "redo", undo.GetRedoName() } } },
					{ "sceneLoad", DescribeSceneLoad(context) },
					{ "memory", DescribeProcessMemory() } };
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
			} });

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
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Play mode
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "play.start",
			"Runs a copy of the scene with scripts, physics and audio. Changes made while playing are discarded by play.stop. When the project's "
			"scripts need building first (their sources changed since the module was built, or scenes use scripts that are not built or "
			"loaded), it builds them, then plays the new code: the answer comes once the build finished (a failed build fails play.start with "
			"its first errors; script.status lists every diagnostic). A build already running is waited for. Returns the play state and, when it "
			"built, built: {build, seconds, reason}.",
			ObjectSchema({ { "build", BoolSchema("Build the scripts first when they need it (default true); false plays what is loaded") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool build = arguments.GetBool("build", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				// A running scene refuses (Play says so) before anything is built.
				if (build && !context.IsPlaying() && (context.GetScriptBuilder().IsRunning() || context.GetPlayBuildReason()))
					return StartPlayAfterBuild(context);
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
