#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/RecentProjects.h"
#include "Editor/SceneRecovery.h"
#include "Editor/ViewportRenderer.h"

#include <Strata/Renderer/RenderSceneGPU.h>
#include <Strata/Renderer/Renderer.h>

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scripting/ScriptEngine.h>

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
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

		// The sections editor.status reports itself (besides the status providers'), in the order its description lists them.
		constexpr std::array<std::string_view, 6> c_StatusSections = { "engine", "project", "selection", "undo", "sceneLoad", "memory" };

		// Every key the built-in part of editor.status can have: a status provider of that name is not reported.
		bool IsBuiltinStatusSection(std::string_view section)
		{
			static constexpr std::array<std::string_view, 10> c_Keys = { "engineVersion", "engineCommit", "platform", "scene", "play", "errors",
				"saving", "selection", "undo", "project" };
			return std::find(c_StatusSections.begin(), c_StatusSections.end(), section) != c_StatusSections.end()
				|| std::find(c_Keys.begin(), c_Keys.end(), section) != c_Keys.end();
		}

		nlohmann::json DescribeProjectBriefly(const EditorContext& context)
		{
			const Ref<Project>& project = context.GetProject();
			if (!project)
				return { { "open", false }, { "name", nullptr }, { "path", nullptr } };
			return { { "open", true }, { "name", project->GetConfig().Name }, { "path", FileSystem::ToUTF8(project->GetProjectFile()) } };
		}

		// What went wrong that an agent should look at: errors logged since it last read the log, the last script build's
		// failure, the script fault that stopped play mode, imports that failed without anyone waiting for them.
		nlohmann::json DescribeErrors(const EditorContext& context)
		{
			const ScriptBuildResult& build = context.GetScriptBuilder().GetLastResult();
			const size_t buildErrors = CountScriptBuildErrors(build);
			const Ref<ScriptEngine>& engine = context.GetScriptEngine();
			const std::optional<ScriptFault> fault = engine ? engine->GetFault() : std::nullopt;
			nlohmann::json scriptFault = nullptr;
			if (fault)
				scriptFault = fmt::format("{}::{} on '{}': {}", fault->ClassName, fault->Method, fault->EntityName, fault->Description);
			return {
				{ "unread", Log::GetBuffer().CountEntries(context.GetLogReadSequence(), LogLevel::Error) },
				{ "scriptBuild", { { "failed", build.ID != 0 && !build.Success && !build.Cancelled }, { "errors", buildErrors } } },
				{ "scriptFault", std::move(scriptFault) },
				{ "importFailures", context.GetImportFailureCount() } };
		}

		nlohmann::json DescribeFrameTimeStats(const FrameTimeStats& stats)
		{
			return { { "avgMs", stats.Average }, { "p50Ms", stats.P50 }, { "p95Ms", stats.P95 }, { "maxMs", stats.Max } };
		}

		// editor.perf's gpu: the frames with GPU times, their busy time, how late the times came, each GPU stage, and what
		// bounds the frames.
		nlohmann::json DescribeGPUProfile(const FrameProfileSummary& summary)
		{
			nlohmann::json stages = nlohmann::json::object();
			for (const FrameStageSummary& stage : summary.Stages)
			{
				if (stage.Kind == FrameStageKind::GPU)
					stages[stage.Name] = DescribeFrameTimeStats(stage.Time);
			}
			return {
				{ "frames", summary.GPU.Frames },
				{ "latencyFrames", summary.GPU.LatencyFrames },
				{ "busy", DescribeFrameTimeStats(summary.GPU.Busy) },
				{ "stages", std::move(stages) },
				{ "boundBy", CommandUtils::GetFrameBoundName(summary.GPU.BoundBy) } };
		}

	}

	namespace CommandUtils
	{

		nlohmann::json DescribeFrameProfile(const FrameProfileSummary& summary, const JobSystemStats& jobs)
		{
			nlohmann::json stages = nlohmann::json::object();
			for (const FrameStageSummary& stage : summary.Stages)
			{
				if (stage.Kind != FrameStageKind::CPU)
					continue; // The main thread's stages only
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
				{ "stages", std::move(stages) },
				{ "gpu", DescribeGPUProfile(summary) } };
		}

		nlohmann::json DescribeGeometryPool()
		{
			const Ref<GeometryPool> pool = Renderer::GetGeometryPool();
			if (!pool)
				return nullptr;
			const GeometryPoolStats stats = pool->GetStats();
			constexpr double c_BytesPerMB = 1024.0 * 1024.0;
			return {
				{ "usedMB", static_cast<double>(stats.UsedBytes) / c_BytesPerMB },
				{ "capacityMB", static_cast<double>(stats.CapacityBytes) / c_BytesPerMB },
				{ "allocations", stats.Allocations },
				{ "fragmentation", stats.VertexFragmentation },
				{ "moves", stats.Moves },
				{ "growths", stats.Growths } };
		}

		const char* GetFrameBoundName(FrameBound bound)
		{
			switch (bound)
			{
				case FrameBound::CPU: return "cpu";
				case FrameBound::GPU: return "gpu";
				case FrameBound::Capped: return "capped";
				case FrameBound::Unknown: break;
			}
			return "unknown";
		}

		nlohmann::json DescribeRenderStats(const SceneRendererStats& frameStats, const SceneRendererGPUStats& gpu)
		{
			// The GPU-driven path's counts come from the GPU, frames later.
			const SceneRendererStats stats = AddGPUCounts(frameStats, gpu);
			nlohmann::json lods = nlohmann::json::array();
			for (const uint32_t count : stats.InstancesPerLOD)
				lods.push_back(count);
			nlohmann::json gpuMs = nlohmann::json::object();
			for (const GPUStageTime& stage : gpu.StageMs)
				gpuMs[FrameProfiler::Get().GetStageName(stage.Stage)] = stage.Milliseconds;
			return {
				{ "gpuDriven", stats.GPUDriven },
				{ "gpuCounts", stats.GPUDriven && gpu.HasCounters },
				{ "feedbackRequests", stats.FeedbackRequests },
				{ "drawCommands", stats.DrawCommands },
				{ "draws", stats.DrawCalls },
				{ "opaqueDraws", stats.OpaqueDrawCalls },
				{ "shadowDraws", stats.ShadowDrawCalls },
				{ "instances", stats.Instances },
				{ "shadowCasters", stats.ShadowCasters },
				{ "triangles", stats.Triangles },
				{ "visibleProxies", stats.VisibleProxies },
				{ "occludedProxies", stats.OccludedProxies },
				{ "occludedCasters", stats.OccludedCasters },
				{ "cachedCascades", std::popcount(stats.CachedCascadeMask) },
				{ "culledProxies", stats.CulledInstances },
				{ "instancesPerLod", std::move(lods) },
				{ "lights", stats.Lights },
				{ "droppedLights", stats.DroppedLights },
				{ "materials", stats.Materials },
				{ "pendingAssets", stats.PendingAssets },
				{ "extractMs", stats.ExtractMs },
				{ "sortMs", stats.SortMs },
				{ "proxyUpdates", stats.ProxyUpdates },
				{ "instanceBytesUploaded", stats.InstanceBytesUploaded },
				{ "deferredInstanceBytes", stats.DeferredInstanceBytes },
				{ "sizeCulledProxies", stats.SizeCulledProxies },
				{ "geometryPool", DescribeGeometryPool() },
				// The scenes' GPU copies (a playing scene's clone doubles its scene's while it plays).
				{ "sceneMirrorMB", Renderer::IsInitialized() ? static_cast<double>(Renderer::GetRenderSceneGPUs().GetBufferBytes()) / (1024.0 * 1024.0) : 0.0 },
				{ "gpuMs", std::move(gpuMs) },
				{ "gpuStatsFrame", gpu.HasTimes ? nlohmann::json(gpu.Frame) : nlohmann::json(nullptr) } };
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
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Activity
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.activity",
			"What people, agents and scripts did in this editor: the activity journal of every command run (the latest 2,000, within 8 MB), oldest "
			"first after since. Each entry: {id, parent (the command that ran it), time (milliseconds since 1970-01-01 UTC), source (UI, "
			"Automation, CommandScript or Internal), client (an automation client's name), command, summary (in a few words, as the Activity panel "
			"lists it: the undo step with the entities' names, the error's first line, the warning, or done), mutating, playing (ran while the scene played), "
			"runningCopy (changed the playing scene's running copy, which play.stop discarded), parameters (parametersTruncated when too large to keep), status (Pending, Succeeded, Failed), result (a summary) or error {kind, "
			"message}, warning, durationMs, frames (of a command that took several), entities (that its undo step touched, at most 256) and "
			"entityCount, undoStep}. Page with since = the returned latest while more is true, or with newest: true (the latest first) with "
			"before = the returned oldest; editor.activity.export turns entries into a command script.",
			ObjectSchema({
				{ "since", IntegerSchema("Return entries with an id above this (default 0: from the oldest kept)", 0, std::numeric_limits<int64_t>::max()) },
				{ "before", IntegerSchema("Return entries with an id below this (default: none)", 1, std::numeric_limits<int64_t>::max()) },
				{ "newest", BoolSchema("Newest first: the latest entries (default false: oldest first)") },
				{ "limit", IntegerSchema("Most entries to return (default 100)", 1, static_cast<int64_t>(CommandJournal::c_MaxEntries)) },
				{ "source", { { "type", "string" }, { "enum", { "UI", "Automation", "CommandScript", "Internal" } },
					{ "description", "Only entries of this source" } } },
				{ "command", StringSchema("Only entries of this command, or of a group with a prefix ending in '.' (\"entity.\")") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t since = arguments.GetInt("since", 0, 0, std::numeric_limits<int64_t>::max());
				constexpr int64_t c_NoBound = std::numeric_limits<int64_t>::max();
				const uint64_t before = static_cast<uint64_t>(arguments.GetInt("before", c_NoBound, 1, c_NoBound));
				const bool newest = arguments.GetBool("newest", false);
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
				const auto matches = [&](const CommandJournalEntry& entry)
				{
					return entry.Id > static_cast<uint64_t>(since) && entry.Id < before && entry.Id != self && (!source || entry.Source == *source)
						&& (command.empty() || (group ? entry.Command.starts_with(command) : entry.Command == command));
				};
				nlohmann::json entries = nlohmann::json::array();
				bool more = false;
				nlohmann::json result;
				if (newest)
				{
					// Newest first, so paging with before = oldest never skips any.
					uint64_t oldest = 0;
					const std::deque<CommandJournalEntry>& journalEntries = journal.GetEntries();
					for (auto entry = journalEntries.rbegin(); entry != journalEntries.rend() && entry->Id > static_cast<uint64_t>(since); ++entry)
					{
						if (!matches(*entry))
							continue;
						if (entries.size() == static_cast<size_t>(limit))
						{
							more = true;
							break;
						}
						oldest = entry->Id;
						entries.push_back(CommandJournal::Describe(*entry));
					}
					result = { { "entries", std::move(entries) }, { "oldest", oldest != 0 ? nlohmann::json(oldest) : nlohmann::json(nullptr) },
						{ "more", more } };
				}
				else
				{
					uint64_t latest = static_cast<uint64_t>(since);
					for (const CommandJournalEntry& entry : journal.GetEntries())
					{
						if (entry.Id >= before)
							break;
						if (!matches(entry))
							continue;
						if (entries.size() == static_cast<size_t>(limit))
						{
							more = true;
							break;
						}
						latest = entry.Id;
						entries.push_back(CommandJournal::Describe(entry));
					}
					result = { { "entries", std::move(entries) }, { "latest", latest }, { "more", more } };
				}
				result["journal"] = { { "entries", journal.GetEntries().size() }, { "bytes", journal.GetBytes() }, { "latest", journal.GetLatestId() } };
				return EditorCommandResult::Ok(std::move(result));
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

		registry.Register(MayDefer({ "scene.recoveryStatus",
			"The recovery data of the open project (.strata/Recovery): unsaved scene changes an editor left behind when it ended without closing "
			"the scene (a crash), or that were discarded on purpose (force, Don't Save) and kept anyway. Returns entries, newest first, each {id, "
			"scene (null for a scene never saved), sceneName, scenePath, time (milliseconds since 1970-01-01 UTC), discarded, entityCount, complete "
			"(a snapshot of every entity), applicable, problem (why it cannot be applied, e.g. the scene was saved since)}, and the journal of the "
			"edited scene {active, file, writes, lastWrite, pending (changes not written yet: they are written about 2 s after edits stop), "
			"pendingSteps (undo steps waiting for that write), busy, "
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
			}, EditorCommandKind::Query, EditorCommandResults::Session }));

		registry.Register(MayDefer({ "scene.recover",
			"Applies or discards recovery data (scene.recoveryStatus). apply: opens the scene the changes were made to in the background as "
			"scene.open does (a scene never saved: a new scene of its name) and, once it is in, turns it into what the edits left, as one undo "
			"step back to the saved scene; the scene stays modified (save it to keep the changes). It refuses when the scene file changed since "
			"the changes were made (applicable false), while playing, while a scene is being opened (scene.loadStatus), and while the edited "
			"scene has unsaved changes it would discard unless force is true; it waits for saves in flight (scene.save) first. discard: moves "
			"the data to "
			".strata/Recovery/Discarded, where it is not offered again. Returns {recovered | discarded: id, ...}; apply also scene, sceneName, "
			"entities (how many it restored or removed) and discardedChanges.",
			ObjectSchema({
				{ "scene", StringSchema("The id of an entry of scene.recoveryStatus, or a scene asset (handle or path) to use its newest applicable entry") },
				{ "action", { { "type", "string" }, { "enum", { "apply", "discard" } }, { "description", "\"apply\" the changes or \"discard\" them" } } },
				{ "force", ForceSchema() } },
				{ "scene", "action" }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
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

				// The entries come from the scan the project's opening (or scene.recoveryStatus) started: wait for it first. Then
				// the scene opens in the background like scene.open's (SceneLoad: its load's ID), and the changes apply once it is in.
				struct Progress
				{
					Ref<SceneRecovery::Load> Load;
					bool Begun = false;
					uint64_t SceneLoad = 0;
					bool Discarded = false;
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
						std::string error;
						if (!progress->Begun)
						{
							const bool replaces = !data.Entry.Scene.IsValid() || context.GetSceneHandle() != data.Entry.Scene || context.IsSceneModified();
							progress->Discarded = replaces && context.IsSceneModified();
							if (!context.BeginRecovery(data, force, &progress->SceneLoad, &error))
								return EditorCommandResult::Fail(error);
							progress->Begun = true;
						}
						if (progress->SceneLoad != 0)
						{
							const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
							if (status.ID != progress->SceneLoad)
								return EditorCommandResult::Fail("Recovering was cancelled: another scene is being opened", EditorCommandError::Cancelled);
							if (!status.IsFinished())
								return std::nullopt;
							if (status.State == SceneLoadState::Cancelled)
								return EditorCommandResult::Fail(status.Error, EditorCommandError::Cancelled);
							if (status.State == SceneLoadState::Failed)
								return EditorCommandResult::Fail(status.Error);
						}
						size_t applied = 0;
						if (!context.ApplyRecovery(data, &applied, &error))
							return EditorCommandResult::Fail(error);
						return EditorCommandResult::Ok({
							{ "recovered", data.Entry.Id },
							{ "scene", data.Entry.Scene.IsValid() ? UUIDToJson(data.Entry.Scene) : nlohmann::json(nullptr) },
							{ "sceneName", data.Entry.SceneName },
							{ "entities", applied },
							{ "discardedChanges", progress->Discarded } });
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
					// A scene being opened would replace what is recovered (BeginRecovery checks again once the data is read).
					std::string loading;
					if (!context.CheckSceneEditable(&loading))
						return EditorCommandResult::Fail(loading);
					progress->Load = recovery.Read(entry->Id, context.GetSceneFile(entry->Scene).first);
					return std::nullopt;
				});
			}), EditorCommandKind::Session, EditorCommandResults::Content, true }));

		////////////////////////////////////////////////////////////////////////////////
		// Editor
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "editor.status",
			"Overview of the editor; start here to orient yourself. The default answer is compact: engineVersion, project {open, name, path}, "
			"scene {name, scene, modified, entityCount}, play {state, paused, pendingSteps, running}, selection {count, primary}, undo {undo, "
			"redo}, errors {unread (errors logged since the last log.read), scriptBuild {failed, errors}, scriptFault, importFailures}, assets "
			"{ready, loading, importing, scanning, failed}, saving and editor {launcher}. sections adds whole sections: engine (engineCommit, "
			"platform), project (directories, start scene, export settings), selection (with inspectedAsset), undo (position, count), sceneLoad "
			"(the scene scene.open or project.open is opening: {scene, startScene, state, progress}, null when none), memory (privateMB, "
			"workingSetMB and their peaks) and the sections of editor parts: automation (port, clients, connectedClients with the names agents "
			"gave, pending requests and commands), assets (memory and budgets per pool, streaming queue, uploads; see asset.stats), game (the "
			"running game's window requests, cursor and save directory), recovery (the edited scene's journal of unsaved changes and how many "
			"recovery entries apply; see scene.recoveryStatus) and editor (window, frame rate, GPU). sections: [\"all\"] returns everything.",
			ObjectSchema({ { "sections", { { "type", "array" }, { "items", { { "type", "string" } } }, { "uniqueItems", true },
				{ "description", "Sections to add to the compact answer: engine, project, selection, undo, sceneLoad, memory, automation, assets, "
					"game, recovery, editor, or all" } } } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::vector<std::string> sections = arguments.GetStringArray("sections");
				if (!arguments.IsValid())
					return arguments.Fail();
				const bool all = std::find(sections.begin(), sections.end(), "all") != sections.end();
				for (const std::string& section : sections)
				{
					if (section != "all" && !IsBuiltinStatusSection(section) && !context.GetStatusProviders().contains(section))
					{
						std::string known;
						for (const std::string_view name : c_StatusSections)
							known += fmt::format("{}{}", known.empty() ? "" : ", ", name);
						for (const auto& [name, provider] : context.GetStatusProviders())
							known += fmt::format(", {}", name);
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'sections': unknown section '{}' (sections: {}, all)", section, known));
					}
				}
				const auto wants = [&](std::string_view section)
				{
					return all || std::find(sections.begin(), sections.end(), section) != sections.end();
				};

				Entity primary = context.GetPrimarySelection();
				const UndoStack& undo = context.GetUndoStack();
				nlohmann::json status = { { "engineVersion", c_EngineVersion } };
				if (wants("engine"))
				{
					status["engineCommit"] = c_EngineCommit;
					status["platform"] = std::string(Platform::GetName());
				}
				status["project"] = wants("project") ? DescribeProject(context) : DescribeProjectBriefly(context);
				status["scene"] = {
					{ "name", context.GetEditScene()->GetName() },
					{ "scene", context.GetSceneHandle().IsValid() ? UUIDToJson(context.GetSceneHandle()) : nlohmann::json(nullptr) },
					{ "modified", context.IsSceneModified() },
					{ "entityCount", context.GetActiveScene()->GetEntityCount() } };
				status["play"] = DescribePlayState(context);
				status["play"]["pendingSteps"] = context.GetPendingSteps();
				status["play"]["running"] = DescribeRunningScene(context);
				status["selection"] = {
					{ "count", context.GetSelection().size() },
					{ "primary", primary ? UUIDToJson(primary.GetUUID()) : nlohmann::json(nullptr) } };
				if (wants("selection"))
					status["selection"]["inspectedAsset"] = context.GetInspectedAsset().IsValid() ? UUIDToJson(context.GetInspectedAsset()) : nlohmann::json(nullptr);
				status["undo"] = { { "undo", undo.GetUndoName() }, { "redo", undo.GetRedoName() } };
				if (wants("undo"))
				{
					status["undo"]["position"] = undo.GetPosition();
					status["undo"]["count"] = undo.GetHistory().size();
				}
				status["errors"] = DescribeErrors(context);
				// Saves running in the background (scene.save, scene.saveAs), so that agents see a save they started without waiting.
				status["saving"] = DescribeSaves(context);
				if (wants("sceneLoad"))
					status["sceneLoad"] = DescribeSceneLoad(context);
				if (wants("memory"))
					status["memory"] = DescribeProcessMemory();
				// Sections of other editor parts; they never replace the built-in ones.
				for (const auto& [section, provider] : context.GetStatusProviders())
				{
					if (IsBuiltinStatusSection(section) || status.contains(section))
						continue;
					if (wants(section))
						status[section] = provider.Full();
					else if (provider.Compact)
						status[section] = provider.Compact();
				}
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

		registry.Register(MayDefer({ "editor.quit",
			"Closes the editor after answering (pending commands are cancelled). Fails while the scene has unsaved changes unless force is "
			"true; save first with scene.save or scene.saveAs. Waits for saves in flight first, also with force: a save is never lost.",
			ObjectSchema({ { "force", ForceSchema() } }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
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
			}), EditorCommandKind::Session }));

		////////////////////////////////////////////////////////////////////////////////
		// Frames
		////////////////////////////////////////////////////////////////////////////////

		registry.Register(MayDefer({ "editor.wait",
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
				return EditorCommandResult::Defer([frames, remaining = frames, frameTimes = FrameTimeRecorder(firstMeasured, static_cast<size_t>(frames))](
					EditorContext& context) mutable -> std::optional<EditorCommandResult>
				{
					frameTimes.Update(context);
					if (--remaining > 0)
						return std::nullopt;
					nlohmann::json result = { { "frames", frames } };
					if (!frameTimes.IsEmpty())
						result["frameTimes"] = frameTimes.Describe();
					return EditorCommandResult::Ok(std::move(result));
				});
			}, EditorCommandKind::Query }));

		registry.Register({ "editor.perf",
			"Where the editor's frames go: statistics of the latest frames the frame profiler recorded (at most 600). Returns frames (how many "
			"were summarized), frame (the CPU time of a frame without the waits for the GPU and the display, like the status bar's frame time), "
			"framesPerSecond, stages (per stage of the main thread: Input, Assets.Update, Assets.Finalize, Scene.Load (swapping in a scene that "
			"finished loading, creating sub-scenes' entities), Scene.UI (the game UI's input), Scene.Scripts, Scene.Animation, Scene.Physics, "
			"Scene.Audio, Scene.Transforms, Render.Extract, Render.Submit, Render.UI (the game UI pass), Editor.UI and Present, plus stages modules registered; each its self "
			"time, nested stages excluded, with inFrameTime false for Present: presenting after the frame, and the wait for the display's image at "
			"the back buffer's first use inside it; neither counts as the frame's work), stageSum (the "
			"stages inside the frame time added up per frame), untimed (the rest of each frame, work no stage accounts for) and jobs (worker, "
			"I/O and background threads (imports, exports), how busy they were in percent, running and queued jobs now). gpu is the GPU's side of "
			"the same frames, known 2-3 frames late: frames (with GPU times), latencyFrames, busy (the GPU stages added up per frame), stages (per GPU "
			"stage: GPU.Uploads, GPU.Setup, GPU.Cull, GPU.Shadows, GPU.Prepass, GPU.AO, GPU.Opaque (with the sky), GPU.Transparent, GPU.Post, "
			"GPU.Overlays, GPU.UI (the game UI), GPU.ImGui) and boundBy: gpu when the GPU's median busy time takes at least 90% of the median frame interval and exceeds "
			"the CPU's frame time, cpu when the CPU's does, capped when neither does (vsync or a frame-rate cap sets the pace), unknown without GPU "
			"times. renderStats is the viewport's latest frame (null while it renders none): gpuDriven (the GPU culled the frame and built its draws: "
			"its counts are the GPU's, of a frame 2-3 frames older, once gpuCounts is true; transparent surfaces are the CPU's), feedbackRequests (its "
			"asset requests came from the GPU's visibility feedback of an earlier frame, not from the CPU walking the view), drawCommands (draw "
			"calls the CPU recorded), draws (instanced draws of the view), opaqueDraws, shadowDraws, instances, shadowCasters, triangles, "
			"visibleProxies, occludedProxies (in the frustum but hidden behind what the view draws: left out on the GPU-driven path), occludedCasters "
			"(shadow casters hidden from the light behind other casters, per cascade: left out of those cascades on the GPU-driven path), cachedCascades "
			"(shadow cascades kept from the previous frame: nothing reaching them changed), culledProxies, "
			"sizeCulledProxies (in view but below a pixel), instancesPerLod, lights, droppedLights, "
			"materials, pendingAssets, extractMs, sortMs, proxyUpdates, instanceBytesUploaded, deferredInstanceBytes (new mesh renderers waiting for "
			"their upload to the GPU), geometryPool "
			"(the meshes' GPU geometry: usedMB, capacityMB, allocations, fragmentation, moves, growths), sceneMirrorMB (the GPU memory of the scenes' "
			"GPU copies: a playing scene's doubles its scene's), gpuMs "
			"(per GPU stage of the frame gpuStatsFrame names). Times are milliseconds as avgMs, p50Ms and p95Ms (nearest rank) and maxMs.",
			ObjectSchema({ { "frames", IntegerSchema("How many of the latest frames to summarize (default 120)", 1, FrameProfiler::c_DefaultCapacity) } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t frames = arguments.GetInt("frames", c_DefaultPerfFrames, 1, FrameProfiler::c_DefaultCapacity);
				if (!arguments.IsValid())
					return arguments.Fail();
				const FrameProfileSummary summary = FrameProfiler::Get().Summarize(static_cast<uint32_t>(frames));
				nlohmann::json result = DescribeFrameProfile(summary, JobSystem::GetStats());
				// The viewport's latest frame, while the viewport panel renders one.
				const ViewportRenderer* viewport = context.GetViewport().FindRenderer();
				result["renderStats"] = viewport && viewport->GetStats().Rendered
					? DescribeRenderStats(viewport->GetStats(), viewport->GetGPUStats()) : nlohmann::json(nullptr);
				return EditorCommandResult::Ok(std::move(result));
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		////////////////////////////////////////////////////////////////////////////////
		// Log
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "log.read",
			"Log messages (engine, editor and scripts), oldest first after a sequence number, or with newest: true the latest first. Page "
			"oldest first with after = the returned latest, newest first with before = the returned oldest, while more is true. Reading from "
			"level Error or below without a logger marks the errors read for editor.status (errors.unread).",
			ObjectSchema({
				{ "after", IntegerSchema("Only messages with a sequence number above this (default 0)", 0, std::numeric_limits<int64_t>::max()) },
				{ "before", IntegerSchema("Only messages with a sequence number below this (default: none)", 1, std::numeric_limits<int64_t>::max()) },
				{ "newest", BoolSchema("Newest first: the latest messages (default false: oldest first)") },
				{ "logger", StringSchema("Only messages of this logger (exact name, e.g. Strata, App, Script)") },
				{ "limit", IntegerSchema("Maximum number of messages (default 200)", 1, 10000) },
				{ "maxCount", IntegerSchema("The former name of limit", 1, 10000) },
				{ "minLevel", { { "type", "string" }, { "enum", { "Trace", "Info", "Warn", "Error", "Critical" } }, { "description", "Lowest level returned (default Info)" } } } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const uint64_t after = static_cast<uint64_t>(arguments.GetInt("after", 0, 0, std::numeric_limits<int64_t>::max()));
				constexpr int64_t c_NoBound = std::numeric_limits<int64_t>::max();
				const uint64_t before = static_cast<uint64_t>(arguments.GetInt("before", c_NoBound, 1, c_NoBound));
				const bool newest = arguments.GetBool("newest", false);
				const std::string logger = arguments.GetString("logger", "");
				const int64_t maxCount = arguments.GetInt("maxCount", 200, 1, 10000);
				const size_t limit = static_cast<size_t>(arguments.GetInt("limit", maxCount, 1, 10000));
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
				if (arguments.Has("limit") && arguments.Has("maxCount") && parameters.at("limit") != parameters.at("maxCount"))
					arguments.SetError("Give limit or maxCount (its former name), not both");
				if (!arguments.IsValid())
					return arguments.Fail();

				const auto describe = [](const LogEntry& entry)
				{
					return nlohmann::json {
						{ "sequence", entry.Sequence },
						{ "level", LogLevelToString(entry.Level) },
						{ "logger", entry.Logger },
						{ "message", entry.Message },
						{ "time", entry.Timestamp } };
				};
				const auto matches = [&](const LogEntry& entry) { return entry.Level >= minLevel && (logger.empty() || entry.Logger == logger); };
				const std::vector<LogEntry> entries = Log::GetBuffer().GetEntries(after);
				nlohmann::json messages = nlohmann::json::array();
				bool more = false;
				// Errors up to the newest message looked at were returned (or skipped below a higher minLevel or for another
				// logger, which leaves them unread).
				const bool marksRead = minLevel <= LogLevel::Error && logger.empty();
				if (newest)
				{
					// Newest first, so paging with before = oldest never skips any.
					uint64_t oldest = before;
					uint64_t newestSeen = 0;
					for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry)
					{
						if (entry->Sequence >= before)
							continue;
						if (!matches(*entry))
						{
							oldest = entry->Sequence;
							newestSeen = std::max(newestSeen, entry->Sequence);
							continue;
						}
						if (messages.size() == limit)
						{
							more = true;
							break;
						}
						oldest = entry->Sequence;
						newestSeen = std::max(newestSeen, entry->Sequence);
						messages.push_back(describe(*entry));
					}
					if (marksRead)
						context.MarkLogRead(newestSeen);
					nlohmann::json result = { { "messages", std::move(messages) }, { "more", more } };
					result["oldest"] = oldest == static_cast<uint64_t>(c_NoBound) ? nlohmann::json(nullptr) : nlohmann::json(oldest);
					return EditorCommandResult::Ok(std::move(result));
				}

				// The oldest messages after `after` come first, so paging with after = latest never skips any.
				uint64_t latest = after;
				for (const LogEntry& entry : entries)
				{
					if (entry.Sequence >= before)
						break;
					if (!matches(entry))
					{
						latest = entry.Sequence;
						continue;
					}
					if (messages.size() == limit)
					{
						more = true;
						break;
					}
					latest = entry.Sequence;
					messages.push_back(describe(entry));
				}
				if (marksRead)
					context.MarkLogRead(latest);
				return EditorCommandResult::Ok({ { "messages", std::move(messages) }, { "latest", latest }, { "more", more } });
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}

}
