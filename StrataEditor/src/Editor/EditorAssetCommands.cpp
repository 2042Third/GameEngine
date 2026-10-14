#include "Editor/AssetEdit.h"
#include "Editor/AssetFileAction.h"
#include "Editor/AssetReferences.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameExport.h"
#include "Editor/ProjectSamples.h"
#include "Editor/ProjectTemplates.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/PrefabInstances.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		EditorAssetManager* RequireAssets(EditorContext& context, std::string* outError)
		{
			EditorAssetManager* assets = context.GetAssetManager();
			if (!assets && outError)
				*outError = "No project is open (project.open or project.create)";
			return assets;
		}

		nlohmann::json DescribeAsset(const AssetMetadata& metadata)
		{
			return {
				{ "asset", UUIDToJson(metadata.Handle) },
				{ "path", metadata.Path },
				{ "name", metadata.Name },
				{ "type", AssetTypeToString(metadata.Type) },
				{ "parent", metadata.IsSubAsset() ? UUIDToJson(metadata.Parent) : nlohmann::json(nullptr) } };
		}

		std::span<const uint8_t> AsBytes(const std::string& text)
		{
			return { reinterpret_cast<const uint8_t*>(text.data()), text.size() };
		}

		// A folder parameter as the asset directory's paths are written: '/' separators, no "./" before or '/' after.
		std::string NormalizeFolderPath(std::string path)
		{
			std::replace(path.begin(), path.end(), '\\', '/');
			while (path.rfind("./", 0) == 0)
				path.erase(0, 2);
			while (!path.empty() && path.back() == '/')
				path.pop_back();
			return path;
		}

		// A source asset of the project (a file in its asset directory): sub-assets and built-in assets have no import of
		// their own.
		bool CheckSourceAsset(const EditorAssetManager& assets, AssetHandle handle, std::string* outError)
		{
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(handle);
			if (!metadata || metadata->IsBuiltin())
			{
				if (outError)
					*outError = fmt::format("{} is not a file of the project", metadata ? metadata->Name : handle.ToString());
				return false;
			}
			if (metadata->IsSubAsset())
			{
				const std::optional<AssetMetadata> parent = assets.GetMetadata(metadata->Parent);
				if (outError)
					*outError = fmt::format("'{}' is imported with '{}': import that asset instead", metadata->Name, parent ? parent->Path : metadata->Parent.ToString());
				return false;
			}
			return true;
		}

		nlohmann::json DescribeImport(const EditorAssetManager& assets, AssetHandle handle)
		{
			const AssetImportInfo info = assets.GetImportInfo(handle);
			return {
				{ "asset", UUIDToJson(handle) },
				{ "importSettings", assets.GetImportSettings(handle) },
				{ "imported", info.Imported },
				{ "importError", info.Error },
				{ "importWarnings", info.Warnings } };
		}

		// asset.import's answer once its imports finished: the items, and a failure (with them in its data) when one failed.
		EditorCommandResult DescribeExternalImport(const EditorAssetManager& assets, const ExternalImport& import)
		{
			nlohmann::json items = nlohmann::json::array();
			std::vector<std::string> errors;
			for (const ExternalImportItem& item : import.GetItems())
			{
				nlohmann::json described = { { "source", FileSystem::ToUTF8(item.Source) }, { "path", item.Path }, { "folder", item.Folder } };
				nlohmann::json list = nlohmann::json::array();
				for (AssetHandle handle : item.Assets)
				{
					if (const std::optional<AssetMetadata> metadata = assets.GetMetadata(handle))
						list.push_back({ { "asset", UUIDToJson(handle) }, { "path", metadata->Path }, { "type", AssetTypeToString(metadata->Type) } });
				}
				described["assets"] = std::move(list);
				if (!item.Error.empty())
				{
					described["error"] = item.Error;
					described["rolledBack"] = item.RolledBack;
					errors.push_back(item.Error);
				}
				items.push_back(std::move(described));
			}
			const std::vector<ExternalImportItem>& all = import.GetItems();
			if (!errors.empty())
			{
				const std::string message = all.size() == 1 ? fmt::format("Import failed: {}", errors.front())
					: fmt::format("{} of {} imports failed; the first: {}", errors.size(), all.size(), errors.front());
				return EditorCommandResult::Fail(message, { { "items", std::move(items) } });
			}
			// The first file's own asset, as asset.import answered for one file.
			nlohmann::json result = nlohmann::json::object();
			if (!all.empty() && !all.front().Assets.empty())
			{
				if (const std::optional<AssetMetadata> metadata = assets.GetMetadata(all.front().Assets.front()))
					result = DescribeAsset(*metadata);
			}
			result["importError"] = "";
			result["items"] = std::move(items);
			return EditorCommandResult::Ok(std::move(result));
		}

		// Answers a re-import once the background import of `handle` is applied (the editor's frames call
		// EditorAssetManager::Update), or at once with `wait` false. The import's failure fails the command.
		EditorCommandResult FinishImport(EditorAssetManager& assets, AssetHandle handle, bool wait)
		{
			if (!wait)
			{
				nlohmann::json result = DescribeImport(assets, handle);
				result["importing"] = assets.IsImportPending(handle);
				return EditorCommandResult::Ok(std::move(result));
			}
			const std::weak_ptr<AssetManagerBase> started = assets.weak_from_this();
			return EditorCommandResult::Defer([started, handle](EditorContext& context) -> std::optional<EditorCommandResult>
			{
				const Ref<AssetManagerBase> manager = started.lock();
				EditorAssetManager* current = context.GetAssetManager();
				if (!manager || manager.get() != current)
					return EditorCommandResult::Fail("The project was closed before the import finished");
				if (current->IsImportPending(handle))
					return std::nullopt;
				if (!current->GetMetadata(handle))
					return EditorCommandResult::Fail("The asset was deleted before its import finished");
				const AssetImportInfo info = current->GetImportInfo(handle);
				if (!info.Error.empty())
					return EditorCommandResult::Fail(fmt::format("Import failed: {}", info.Error));
				nlohmann::json result = DescribeImport(*current, handle);
				result["importing"] = false;
				return EditorCommandResult::Ok(std::move(result));
			});
		}

		// Adds what became of the start scene to a result: startSceneOpened, and startSceneError when it was not opened.
		nlohmann::json DescribeStartScene(nlohmann::json value, bool opened, const std::string& error)
		{
			value["startSceneOpened"] = opened;
			if (!opened && !error.empty())
				value["startSceneError"] = error;
			return value;
		}

		// Answers a command that opened a project (EditorContext::BeginOpenProject) once its start scene is open (it loads in
		// the background, so the frames go on), or at once for a project without one. The project stays open whatever becomes
		// of its start scene: the result says {startScene, startSceneOpened, startSceneError?, seconds} besides `value`.
		EditorCommandResult FinishProjectOpen(EditorContext& context, nlohmann::json value)
		{
			const AssetHandle startScene = context.GetProject()->GetConfig().StartScene;
			value["startScene"] = startScene.IsValid() ? UUIDToJson(startScene) : nlohmann::json(nullptr);
			const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
			if (!startScene.IsValid())
				return EditorCommandResult::Ok(DescribeStartScene(std::move(value), false, std::string()));
			// The start scene could not even start loading (BeginOpenProject recorded why as a failed load).
			if (!context.IsSceneLoading())
				return EditorCommandResult::Ok(DescribeStartScene(std::move(value), false, status.Error));

			const uint64_t id = status.ID;
			const std::weak_ptr<Project> project = context.GetProject();
			return EditorCommandResult::Defer([id, project, value = std::move(value)](EditorContext& context) mutable -> std::optional<EditorCommandResult>
			{
				if (project.lock() != context.GetProject())
					return EditorCommandResult::Fail("Another project was opened (or the project was closed) before its start scene was open",
						EditorCommandError::Cancelled);
				const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
				if (status.ID != id)
					return EditorCommandResult::Ok(DescribeStartScene(std::move(value), false, "Another scene was opened instead (scene.open)"));
				if (!status.IsFinished())
					return std::nullopt;
				value["seconds"] = status.Seconds;
				return EditorCommandResult::Ok(DescribeStartScene(std::move(value), status.State == SceneLoadState::Ready, status.Error));
			});
		}

	}

	void RegisterAssetCommands(EditorCommandRegistry& registry)
	{
		////////////////////////////////////////////////////////////////////////////////
		// Project
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "project.info", "The open project: name, directories and start scene (null fields without a project).", ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				return EditorCommandResult::Ok(DescribeProject(context));
			}, EditorCommandKind::Query });

		registry.Register({ "project.templates",
			"The templates project.create and scene.new accept: id, name and what a project made from it contains. \"basic3d\" starts lit (camera, sun, "
			"procedural sky, ground, post-processing); \"empty\" is the default and has no scene.",
			ObjectSchema({}),
			[](EditorContext&, const nlohmann::json&)
			{
				nlohmann::json templates = nlohmann::json::array();
				for (const ProjectTemplate& projectTemplate : ProjectTemplates::GetAll())
				{
					templates.push_back({
						{ "id", projectTemplate.Id },
						{ "name", projectTemplate.Name },
						{ "description", projectTemplate.Description },
						{ "startScene", ProjectTemplates::HasStartScene(projectTemplate.Id) ? nlohmann::json(ProjectTemplates::c_StartScenePath) : nlohmann::json(nullptr) } });
				}
				return EditorCommandResult::Ok({ { "templates", std::move(templates) }, { "default", ProjectTemplates::c_Empty } });
			}, EditorCommandKind::Query });

		registry.Register({ "project.create",
			"Creates a project in a directory and opens it. With template \"basic3d\" it starts with a saved, lit start scene (Scenes/Main.stscene: Main "
			"Camera, Sun, Sky, Ground, Post Process); the default \"empty\" has no scene (see project.templates). Fails while the edited scene has "
			"unsaved changes unless force is true (save first), before anything is created; waits for saves in flight (scene.save) first. Returns "
			"projectFile, template, startScene and "
			"discardedChanges (whether unsaved changes were discarded).",
			ObjectSchema({
				{ "directory", StringSchema("Absolute directory for the project") },
				{ "name", StringSchema("Project name") },
				{ "template", { { "type", "string" }, { "enum", { ProjectTemplates::c_Empty, ProjectTemplates::c_Basic3D } },
					{ "description", "What the project starts with (default \"empty\"; see project.templates)" } } },
				{ "force", ForceSchema() } }, { "directory", "name" }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string directory = arguments.GetString("directory");
				const std::string name = arguments.GetString("name");
				const std::string templateId = arguments.GetString("template", std::string(ProjectTemplates::c_Empty));
				const bool force = arguments.GetBool("force", false);
				if (arguments.IsValid() && !ProjectTemplates::Find(templateId))
					arguments.SetError(fmt::format("Unknown template '{}' (templates: {})", templateId, ProjectTemplates::ListIds()));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, fmt::format("creating the project '{}'", name)))
					return std::move(*refused);
				const bool discarded = context.IsSceneModified();
				std::string error;
				if (!context.CreateProject(FileSystem::FromUTF8(directory), name, templateId, &error))
					return EditorCommandResult::Fail(error);
				const AssetHandle startScene = context.GetProject()->GetConfig().StartScene;
				return EditorCommandResult::Ok({
					{ "projectFile", FileSystem::ToUTF8(context.GetProject()->GetProjectFile()) },
					{ "template", templateId },
					{ "startScene", startScene.IsValid() ? UUIDToJson(startScene) : nlohmann::json(nullptr) },
					{ "discardedChanges", discarded } });
			}), EditorCommandKind::Session });

		registry.Register({ "project.open",
			"Opens a project file or the project in a directory. Fails while the edited scene has unsaved changes unless force is true (save first); "
			"waits for saves in flight (scene.save) first, also with force. "
			"The project opens at once; its start scene loads in the background like scene.open's (the frames go on, scene.loadStatus reports it) "
			"and the command answers once it is the edited scene: {projectFile, discardedChanges (whether unsaved changes were discarded), "
			"startScene (null when the project has none), startSceneOpened, startSceneError (why it was not opened; the project stays open with an "
			"empty scene), seconds (the start scene's load)}.",
			ObjectSchema({ { "path", StringSchema("Project file (.stproj) or project directory") }, { "force", ForceSchema() } }, { "path" }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, fmt::format("opening the project '{}'", path)))
					return std::move(*refused);
				// A project that cannot be loaded changes nothing (OpenProject reads it before closing the open one).
				const bool modified = context.IsSceneModified();
				std::string error;
				if (!context.BeginOpenProject(FileSystem::FromUTF8(path), &error))
					return EditorCommandResult::Fail(error);
				return FinishProjectOpen(context, { { "projectFile", FileSystem::ToUTF8(context.GetProject()->GetProjectFile()) },
					{ "discardedChanges", modified } });
			}), EditorCommandKind::Session });

		registry.Register({ "project.close",
			"Closes the open project; the editor shows its launcher. Without a project it does nothing. Fails while the edited scene has unsaved "
			"changes unless force is true (save first). Waits for saves in flight (scene.save) first, also with force: a save is never lost. Returns "
			"closed and discardedChanges (whether unsaved changes were discarded).",
			ObjectSchema({ { "force", ForceSchema() } }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				const bool hadProject = context.HasProject();
				if (!hadProject)
					return EditorCommandResult::Ok({ { "closed", false }, { "discardedChanges", false } });
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, "closing the project"))
					return std::move(*refused);
				const bool discarded = context.IsSceneModified();
				context.CloseProject();
				return EditorCommandResult::Ok({ { "closed", true }, { "discardedChanges", discarded } });
			}), EditorCommandKind::Session });

		registry.Register({ "project.samples",
			"The sample projects that come with the editor (finished games to learn from, e.g. Tetris): id, name, description. Open one with "
			"project.openSample, which works on a copy.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				std::string error;
				const std::optional<std::vector<ProjectSample>> samples = ProjectSamples::List(context.GetSamplesDirectory(), &error);
				if (!samples)
					return EditorCommandResult::Fail(error);
				nlohmann::json list = nlohmann::json::array();
				for (const ProjectSample& sample : *samples)
					list.push_back({ { "id", sample.Id }, { "name", sample.Name }, { "description", sample.Description } });
				return EditorCommandResult::Ok({ { "samples", std::move(list) } });
			}, EditorCommandKind::Query });

		registry.Register({ "project.openSample",
			"Copies a sample project (project.samples) into a new directory and opens the copy, like project.open; the sample itself stays unchanged. "
			"The copy leaves out local editor data (.strata): build its scripts with script.build before playing. Fails while the edited scene has "
			"unsaved changes unless force is true (save first), before anything is copied; waits for saves in flight (scene.save) first. It answers "
			"once the start scene is open, with "
			"project.open's fields (projectFile, discardedChanges: whether unsaved changes were discarded, the start scene's) and sample.",
			ObjectSchema({
				{ "sample", StringSchema("The sample's id from project.samples, e.g. \"Tetris\"") },
				{ "directory", StringSchema("Absolute directory for the copy: created, or an existing empty directory") },
				{ "force", ForceSchema() } }, { "sample", "directory" }),
			AfterSaves([](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string id = arguments.GetString("sample");
				const std::string directory = arguments.GetString("directory");
				const bool force = arguments.GetBool("force", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				const std::optional<std::vector<ProjectSample>> samples = ProjectSamples::List(context.GetSamplesDirectory(), &error);
				if (!samples)
					return EditorCommandResult::Fail(error);
				const auto sample = std::find_if(samples->begin(), samples->end(), [&id](const ProjectSample& candidate) { return candidate.Id == id; });
				if (sample == samples->end())
				{
					std::string known;
					for (const ProjectSample& candidate : *samples)
						known += (known.empty() ? "" : ", ") + candidate.Id;
					return EditorCommandResult::InvalidParameters(fmt::format("There is no sample '{}' (samples: {})", id, known.empty() ? "none" : known));
				}
				if (std::optional<EditorCommandResult> refused = CheckUnsavedChanges(context, force, fmt::format("opening the sample '{}'", sample->Id)))
					return std::move(*refused);
				const bool discarded = context.IsSceneModified();
				const std::filesystem::path projectFile = ProjectSamples::Copy(*sample, FileSystem::FromUTF8(directory), &error);
				if (projectFile.empty())
					return EditorCommandResult::Fail(error);
				if (!context.BeginOpenProject(projectFile, &error))
					return EditorCommandResult::Fail(fmt::format("The sample was copied to '{}' but could not be opened: {}", directory, error));
				return FinishProjectOpen(context, { { "sample", sample->Id }, { "projectFile", FileSystem::ToUTF8(projectFile) },
					{ "discardedChanges", discarded } });
			}), EditorCommandKind::Session });

		registry.Register({ "project.setStartScene", "Sets the scene a built game starts with and saves the project file. When it is the "
			"export settings' loading scene (project.setExportSettings), that setting is cleared (loadingSceneCleared: true), as the loading scene "
			"plays while the start scene loads. Returns startScene and loadingSceneCleared.",
			ObjectSchema({ { "scene", AssetSchema("Start scene") } }, { "scene" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				if (!RequireAssets(context, &error))
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle scene = ResolveAsset(context, arguments, "scene", AssetType::Scene);
				if (!arguments.IsValid())
					return arguments.Fail();
				Project& project = *context.GetProject();
				const AssetHandle previous = project.GetConfig().StartScene;
				const AssetHandle previousLoading = project.GetConfig().Export.LoadingScene;
				project.GetConfig().StartScene = scene;
				// The loading scene plays while the start scene loads: the scene that starts now no longer is the loading scene.
				const bool loadingSceneCleared = previousLoading.IsValid() && previousLoading == scene;
				if (loadingSceneCleared)
					project.GetConfig().Export.LoadingScene = UUID::Null();
				if (!project.Save(&error))
				{
					project.GetConfig().StartScene = previous;
					project.GetConfig().Export.LoadingScene = previousLoading;
					return EditorCommandResult::Fail(error);
				}
				return EditorCommandResult::Ok({ { "startScene", UUIDToJson(scene) }, { "loadingSceneCleared", loadingSceneCleared } });
			} });

		registry.Register({ "project.export", "Exports the project as a playable game (runtime executable, asset pack, script module, manifest) into a "
			"directory outside the project, in the background: the editor keeps running, and the command answers when the export finished (it fails at once "
			"when it cannot start, and fails naming the document when a scene, prefab or material saved meanwhile references what the export did not plan "
			"for: export again). The pack is ordered by first use from the start scene; with prune it holds only what the scenes use and what the "
			"project's Export.AlwaysInclude names (project.setExportSettings), and excludedAssets lists what it left out. The game runs the script module "
			"the editor has loaded (build it first with script.build). Returns the files, assetCount, the pack's size, seconds and frameTimes (the editor's "
			"frames from the one that started the export to its end, like editor.wait's).",
			ObjectSchema({
				{ "directory", StringSchema("Absolute output directory outside the project (created when missing)") },
				{ "includeRuntime", BoolSchema("Copy the runtime executable (default true)") },
				{ "includeScriptSymbols", BoolSchema("Copy the script module's debug symbols (PDB) too (default: true, false in Dist builds)") },
				{ "runtime", StringSchema("Runtime executable to copy (default: StrataRuntime next to the editor)") },
				{ "width", IntegerSchema("Window width (default 1280)", 1, 16384) },
				{ "height", IntegerSchema("Window height (default 720)", 1, 16384) },
				{ "fullscreen", BoolSchema("Start in fullscreen (default false)") },
				{ "prune", BoolSchema("Leave out assets no scene uses and Export.AlwaysInclude does not name (default false: scripts may load assets by path)") } },
				{ "directory" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				GameExportOptions options;
				options.Directory = FileSystem::FromUTF8(arguments.GetString("directory"));
				options.IncludeRuntime = arguments.GetBool("includeRuntime", true);
				options.IncludeScriptSymbols = arguments.GetBool("includeScriptSymbols", options.IncludeScriptSymbols);
				options.RuntimeExecutable = FileSystem::FromUTF8(arguments.GetString("runtime", ""));
				options.WindowWidth = static_cast<uint32_t>(arguments.GetInt("width", options.WindowWidth, 1, 16384));
				options.WindowHeight = static_cast<uint32_t>(arguments.GetInt("height", options.WindowHeight, 1, 16384));
				options.Fullscreen = arguments.GetBool("fullscreen", false);
				options.Prune = arguments.GetBool("prune", false);
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.StartGameExport(options, &error))
					return EditorCommandResult::Fail(error);

				// Frames are measured from the one that started the export (its checks, reading the script module), which the
				// editor records at the start of the next frame.
				const Ref<GameExport> exporter = context.GetGameExport();
				const uint64_t firstMeasured = context.GetRecordedFrameCount() + 1;
				return EditorCommandResult::Defer([exporter, frames = uint64_t(0), frameTimes = FrameTimeRecorder(firstMeasured, c_FrameTimeReserve)](
					EditorContext& context) mutable -> std::optional<EditorCommandResult>
				{
					frames++;
					frameTimes.Update(context);
					if (!exporter->IsFinished())
						return std::nullopt;
					if (exporter->GetState() == GameExportState::Cancelled)
						return EditorCommandResult::Fail(exporter->GetError(), EditorCommandError::Cancelled);
					if (exporter->GetState() != GameExportState::Succeeded)
						return EditorCommandResult::Fail(exporter->GetError());

					const GameExportResult& result = exporter->GetResult();
					nlohmann::json excluded = nlohmann::json::array();
					for (const AssetMetadata& metadata : result.ExcludedAssets)
						excluded.push_back(DescribeAsset(metadata));
					nlohmann::json value = {
						{ "executable", result.Executable.empty() ? nlohmann::json(nullptr) : nlohmann::json(FileSystem::ToUTF8(result.Executable)) },
						{ "manifest", FileSystem::ToUTF8(result.Manifest) },
						{ "assetPack", FileSystem::ToUTF8(result.AssetPack) },
						{ "scriptModule", result.ScriptModule.empty() ? nlohmann::json(nullptr) : nlohmann::json(FileSystem::ToUTF8(result.ScriptModule)) },
						{ "assetCount", result.AssetCount },
						{ "excludedAssets", std::move(excluded) },
						{ "packBytes", result.Pack.FileSize },
						{ "storedBytes", result.Pack.StoredBytes },
						{ "seconds", result.Seconds },
						{ "frames", frames } };
					if (!frameTimes.IsEmpty())
						value["frameTimes"] = frameTimes.Describe();
					return EditorCommandResult::Ok(std::move(value));
				});
			}, EditorCommandKind::Session, EditorCommandResults::Content, true });

		registry.Register({ "project.exportStatus", "The running export (project.export) or the last one: {running, state (WaitingForImports, Packing, Copying, "
			"Succeeded, Failed, Cancelled), progress (0 to 1), stage (what it does, or how it ended), directory}; {running: false, state: null} before the "
			"first.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				const Ref<GameExport>& exporter = context.GetGameExport();
				if (!exporter)
					return EditorCommandResult::Ok({ { "running", false }, { "state", nullptr } });
				return EditorCommandResult::Ok({
					{ "running", !exporter->IsFinished() },
					{ "state", GameExportStateToString(exporter->GetState()) },
					{ "progress", exporter->GetProgress() },
					{ "stage", exporter->GetStage() },
					{ "directory", FileSystem::ToUTF8(exporter->GetOptions().Directory) } });
			}, EditorCommandKind::Query, EditorCommandResults::Session });

		registry.Register({ "project.cancelExport", "Cancels the running export (project.export, which then fails as cancelled); files it wrote already stay. "
			"Fails when no export runs.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				const Ref<GameExport>& exporter = context.GetGameExport();
				if (!exporter || exporter->IsFinished())
					return EditorCommandResult::Fail("No export is running");
				exporter->Cancel();
				return EditorCommandResult::Ok({ { "cancelling", true } });
			}, EditorCommandKind::Session });

		registry.Register({ "project.setExportSettings", "Sets how the project exports and saves the project file; settings left out keep their "
			"values. alwaysInclude: assets (\"Prefabs/Enemy.stprefab\") and folders (\"Prefabs\") relative to the asset directory that an export "
			"with prune keeps, with what they use, although no scene uses them (assets scripts load by path); each must name an asset or a folder "
			"of assets. loadingScene: a scene the exported game plays while its start scene loads (null: none, the game shows a cleared frame "
			"meanwhile). Returns both settings.",
			ObjectSchema({
				{ "alwaysInclude", { { "type", "array" }, { "items", { { "type", "string" } } },
					{ "description", "Asset paths and folders relative to the asset directory ('/' separators); empty to keep nothing extra" } } },
				{ "loadingScene", { { "type", { "string", "null" } },
					{ "description", "Scene (handle or path) the game plays while its start scene loads; null for none" } } } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				const nlohmann::json* list = JsonUtils::Find(parameters, "alwaysInclude");
				const nlohmann::json* loading = JsonUtils::Find(parameters, "loadingScene");
				if (!list && !loading)
					return EditorCommandResult::InvalidParameters("Missing parameter 'alwaysInclude' (or 'loadingScene')");
				Project& project = *context.GetProject();
				const ProjectExportSettings previous = project.GetConfig().Export;
				ProjectExportSettings settings = previous;
				if (list)
				{
					if (!list->is_array())
						return EditorCommandResult::InvalidParameters("'alwaysInclude' must be an array of asset paths");
					settings.AlwaysInclude.clear();
					for (const nlohmann::json& entry : *list)
					{
						if (!entry.is_string())
							return EditorCommandResult::InvalidParameters("'alwaysInclude' must be an array of asset paths");
						settings.AlwaysInclude.push_back(entry.get<std::string>());
					}
					// Every entry must name something, as a pruned pack's plan requires.
					if (!assets->CheckAlwaysInclude(settings.AlwaysInclude, &error))
						return EditorCommandResult::InvalidParameters(error);
				}
				if (loading)
				{
					settings.LoadingScene = UUID::Null();
					if (!loading->is_null())
					{
						CommandArguments arguments(parameters);
						settings.LoadingScene = ResolveAsset(context, arguments, "loadingScene", AssetType::Scene);
						if (!arguments.IsValid())
							return arguments.Fail();
						// It plays while the start scene loads: as the start scene the game would start it twice.
						if (settings.LoadingScene == project.GetConfig().StartScene)
						{
							return EditorCommandResult::InvalidParameters("Parameter 'loadingScene': that is the start scene, and the loading scene plays "
								"while the start scene loads; pick another scene (or null for none)");
						}
					}
				}

				project.GetConfig().Export = std::move(settings);
				if (!project.Save(&error))
				{
					project.GetConfig().Export = previous;
					return EditorCommandResult::Fail(error);
				}
				const ProjectExportSettings& saved = project.GetConfig().Export;
				return EditorCommandResult::Ok({ { "alwaysInclude", saved.AlwaysInclude },
					{ "loadingScene", saved.LoadingScene.IsValid() ? UUIDToJson(saved.LoadingScene) : nlohmann::json(nullptr) } });
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Assets
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "asset.list", "Assets of the project (and built-ins), optionally filtered by type and path prefix.",
			ObjectSchema({
				{ "type", StringSchema("Asset type, e.g. \"Texture\", \"Mesh\", \"Material\", \"Prefab\", \"Model\", \"Scene\"") },
				{ "path", StringSchema("Path prefix relative to the asset directory, e.g. \"Models/\"") },
				{ "includeBuiltin", BoolSchema("Include built-in assets (default true)") } }),
			[](EditorContext&, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				AssetType type = AssetType::None;
				if (arguments.Has("type"))
				{
					const std::string typeName = arguments.GetString("type");
					const std::optional<AssetType> parsed = AssetTypeFromString(typeName);
					if (!parsed)
						arguments.SetError(fmt::format("Unknown asset type '{}'", typeName));
					else
						type = *parsed;
				}
				const std::string prefix = arguments.GetString("path", "");
				const bool includeBuiltin = arguments.GetBool("includeBuiltin", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!AssetManager::HasActive())
					return EditorCommandResult::Ok({ { "assets", nlohmann::json::array() } });

				nlohmann::json assets = nlohmann::json::array();
				for (const AssetMetadata& metadata : AssetManager::GetActive()->GetAllMetadata(type))
				{
					if ((!includeBuiltin && metadata.IsBuiltin()) || metadata.Path.rfind(prefix, 0) != 0)
						continue;
					assets.push_back(DescribeAsset(metadata));
				}
				return EditorCommandResult::Ok({ { "assets", std::move(assets) } });
			}, EditorCommandKind::Query, EditorCommandResults::Content, true });

		registry.Register({ "asset.info",
			"An asset's metadata, load state, import errors and warnings, sub-assets, import settings with the description of every setting "
			"(importSettingsProperties: type, range, options; what asset.setImportSettings accepts) and whether an import is running.",
			ObjectSchema({ { "asset", AssetSchema("Asset") } }, { "asset" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				if (!arguments.IsValid())
					return arguments.Fail();
				const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
				nlohmann::json result = DescribeAsset(*manager->GetMetadata(handle));
				result["state"] = AssetStateToString(manager->GetAssetState(handle));
				result["loadError"] = manager->GetAssetError(handle);
				if (EditorAssetManager* assets = context.GetAssetManager())
				{
					const AssetImportInfo info = assets->GetImportInfo(handle);
					nlohmann::json subAssets = nlohmann::json::array();
					for (AssetHandle subAsset : info.SubAssets)
					{
						if (std::optional<AssetMetadata> metadata = assets->GetMetadata(subAsset))
							subAssets.push_back(DescribeAsset(*metadata));
					}
					result["imported"] = info.Imported;
					result["importError"] = info.Error;
					result["importWarnings"] = info.Warnings;
					result["subAssets"] = std::move(subAssets);
					result["importSettings"] = assets->GetImportSettings(handle);
					// What asset.setImportSettings accepts (sub-assets have none of their own).
					nlohmann::json settingsProperties = nlohmann::json::array();
					const AssetImporter* importer = assets->GetImporter(handle);
					if (importer && !manager->GetMetadata(handle)->IsSubAsset())
					{
						for (const PropertyInfo& property : importer->GetSettingsProperties())
							settingsProperties.push_back(DescribeProperty(property));
					}
					result["importSettingsProperties"] = std::move(settingsProperties);
					result["importing"] = assets->IsImportPending(handle);
				}
				return EditorCommandResult::Ok(std::move(result));
			}, EditorCommandKind::Query, EditorCommandResults::Content, true });

		registry.Register({ "asset.import",
			"Copies external files and folders into the project and imports them in the background: the editor keeps running, and the command answers "
			"when every import is done. A file goes into `directory` (\" (n)\" added when its name is taken) with the files it reads (a glTF model's "
			"buffers and images, in a new folder named after it, keeping their layout); a folder goes there with its tree (asset files, their .meta files, "
			"what its models read). Returns {asset, path, name, type (of the first file's asset), items: [{source, path, folder, assets: [{asset, path, "
			"type}], error?, rolledBack?}]}. Fails (Failed) when an import failed, with the items in the error's data; the failed files are removed "
			"again unless keepOnFailure. wait: false answers at once ({importing: true}); asset.info tells when an asset is imported.",
			ObjectSchema({
				{ "file", StringSchema("Absolute path of a file to import") },
				{ "files", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Absolute paths of files to import" } } },
				{ "directories", { { "type", "array" }, { "items", { { "type", "string" } } },
					{ "description", "Absolute paths of folders to import with their trees" } } },
				{ "directory", StringSchema("Target folder relative to the asset directory (default: the root)") },
				{ "keepOnFailure", BoolSchema("Keep the files of failed imports in the project, with their errors (default false: they are removed)") },
				{ "wait", BoolSchema("Answer when the imports are done (default true)") } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				ExternalImportRequest request;
				if (arguments.Has("file"))
					request.Files.push_back(FileSystem::FromUTF8(arguments.GetString("file")));
				auto readPaths = [&](const char* name, std::vector<std::filesystem::path>& outPaths)
				{
					const nlohmann::json* list = JsonUtils::Find(parameters, name);
					if (!list)
						return;
					if (!list->is_array())
					{
						arguments.SetError(fmt::format("Parameter '{}' must be an array of absolute paths", name));
						return;
					}
					for (const nlohmann::json& entry : *list)
					{
						if (!entry.is_string())
						{
							arguments.SetError(fmt::format("Parameter '{}' must be an array of absolute paths", name));
							return;
						}
						outPaths.push_back(FileSystem::FromUTF8(entry.get<std::string>()));
					}
				};
				readPaths("files", request.Files);
				readPaths("directories", request.Directories);
				request.TargetDirectory = arguments.GetString("directory", "");
				request.KeepOnFailure = arguments.GetBool("keepOnFailure", false);
				const bool wait = arguments.GetBool("wait", true);
				if (arguments.IsValid() && request.Files.empty() && request.Directories.empty())
					arguments.SetError("Pass the files to import ('file' or 'files') or folders ('directories')");
				// What names nothing importable is refused at once, before anything is copied.
				for (const std::filesystem::path& file : request.Files)
				{
					if (!arguments.IsValid())
						break;
					if (!FileSystem::IsRegularFile(file))
						arguments.SetError(fmt::format("'{}' is not a file", FileSystem::ToUTF8(file)));
					else if (!AssetImporterRegistry::FindByExtension(StringUtils::ToLower(FileSystem::ToUTF8(file.extension()))))
						arguments.SetError(fmt::format("No importer handles '{}' files ('{}')", FileSystem::ToUTF8(file.extension()), FileSystem::ToUTF8(file)));
				}
				for (const std::filesystem::path& directory : request.Directories)
				{
					if (arguments.IsValid() && !FileSystem::IsDirectory(directory))
						arguments.SetError(fmt::format("'{}' is not a folder", FileSystem::ToUTF8(directory)));
				}
				if (!arguments.IsValid())
					return arguments.Fail();
				const Ref<ExternalImport> import = assets->ImportExternal(request, &error);
				if (!import)
					return EditorCommandResult::Fail(error);
				if (!wait)
					return EditorCommandResult::Ok({ { "importing", true } });

				const std::weak_ptr<AssetManagerBase> started = assets->weak_from_this();
				return EditorCommandResult::Defer([import, started](EditorContext& context) -> std::optional<EditorCommandResult>
				{
					const Ref<AssetManagerBase> manager = started.lock();
					EditorAssetManager* current = context.GetAssetManager();
					if (!manager || manager.get() != current)
						return EditorCommandResult::Fail("The project was closed before the import finished");
					if (!import->IsFinished())
						return std::nullopt;
					if (!import->GetError().empty())
						return EditorCommandResult::Fail(import->GetError());
					return DescribeExternalImport(*current, *import);
				});
			} });

		registry.Register({ "asset.move",
			"Moves or renames an asset (with its .meta and cached import) within the asset directory; its handle and every reference to it stay "
			"valid. One undo step (edit.undo moves it back); a name that differs only in case renames it in place, and its own path changes "
			"nothing. Fails when another file is at the path. Returns {asset, path (the new one)}.",
			ObjectSchema({ { "asset", AssetSchema("Asset to move") }, { "path", StringSchema("New path relative to the asset directory, with the file's extension") } },
				{ "asset", "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				const std::string path = arguments.GetString("path");
				if (arguments.IsValid() && !CheckSourceAsset(*assets, handle, &error))
					arguments.SetError(fmt::format("Parameter 'asset': {}", error));
				if (!arguments.IsValid())
					return arguments.Fail();
				const std::string from = assets->GetMetadata(handle)->Path;
				if (!assets->MoveAsset(handle, path, &error))
					return EditorCommandResult::Fail(error);
				const std::string to = assets->GetMetadata(handle)->Path;
				// Its own path moves nothing: no step.
				if (to != from)
					context.GetUndoStack().Record(CreateScope<AssetMoveAction>(*assets, fmt::format("Move {}", from), handle, from, to));
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "path", to } });
			} });

		registry.Register({ "asset.findReferences",
			"What uses an asset: the scenes, prefabs, models and materials whose recorded references name it (or one of its sub-assets), and the open "
			"scene as it is in memory, with the entities that use it. Returns {asset, path, referencedBy: [{asset, path, type, name?, openScene?, "
			"entities?: [{id, name, component, property}]}]}.",
			ObjectSchema({ { "asset", AssetSchema("Asset") } }, { "asset" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				if (!arguments.IsValid())
					return arguments.Fail();
				const std::vector<AssetHandle> targets = AssetReferences::WithSubAssets(context, { handle });
				const std::vector<AssetReferencer> referencers = AssetReferences::Find(context, targets, { targets.begin(), targets.end() });
				const std::optional<AssetMetadata> metadata = assets->GetMetadata(handle);
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "path", metadata ? metadata->Path : std::string() },
					{ "referencedBy", AssetReferences::ToJson(referencers) } });
			}, EditorCommandKind::Query, EditorCommandResults::Content, true });

		registry.Register({ "asset.delete",
			"Deletes an asset: moves its file, .meta and cached import to the project's trash (.strata/Trash) and removes it from the project, in one undo "
			"step (edit.undo restores it with the same handle, so references to it work again, as long as the edited scene stays: opening or creating "
			"another clears the undo history; the trash keeps the files for 30 days). Fails while scenes, prefabs, models, materials or the open "
			"scene use it, listing them in the error's data (referencedBy, as asset.findReferences returns it); force: true deletes it anyway, leaving "
			"those references missing. The open scene itself cannot be deleted. Returns {asset, path, trash, referencedBy}.",
			ObjectSchema({ { "asset", AssetSchema("Asset to delete") },
				{ "force", BoolSchema("Delete although other assets or the open scene use it (default false)") } }, { "asset" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				const bool force = arguments.GetBool("force", false);
				if (arguments.IsValid() && !CheckSourceAsset(*assets, handle, &error))
					arguments.SetError(fmt::format("Parameter 'asset': {}", error));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (handle == context.GetSceneHandle())
					return EditorCommandResult::Fail("The open scene cannot be deleted; open another scene first");

				const std::string path = assets->GetMetadata(handle)->Path;
				const std::vector<AssetHandle> targets = AssetReferences::WithSubAssets(context, { handle });
				const std::vector<AssetReferencer> referencers = AssetReferences::Find(context, targets, { targets.begin(), targets.end() });
				if (!referencers.empty() && !force)
				{
					return EditorCommandResult::Fail(fmt::format("'{}' is used by {}; delete it with force: true to leave those references missing, or "
						"change them first (asset.findReferences lists them)", path, AssetReferences::Describe(referencers)),
						{ { "referencedBy", AssetReferences::ToJson(referencers) } });
				}
				TrashedAssets trashed;
				if (!assets->MoveToTrash(path, trashed, &error))
					return EditorCommandResult::Fail(error);
				context.GetUndoStack().Record(CreateScope<AssetTrashAction>(context, *assets, fmt::format("Delete {}", path), trashed, force));
				context.NotifyAssetsTrashed(path, false, trashed.Handles.size(), !referencers.empty());
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "path", path }, { "trash", FileSystem::ToUTF8(trashed.Entry) },
					{ "referencedBy", AssetReferences::ToJson(referencers) } });
			} });

		registry.Register({ "asset.createFolder", "Creates a folder (and its parents) in the asset directory, in one undo step. Fails when it exists. "
			"Returns {path} (normalized, '/' separators).",
			ObjectSchema({ { "path", StringSchema("Folder relative to the asset directory, e.g. \"Materials/Metals\"") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!assets->CreateFolder(path, &error))
					return EditorCommandResult::Fail(error);
				const std::string created = NormalizeFolderPath(path);
				context.GetUndoStack().Record(CreateScope<AssetFolderCreateAction>(*assets, created));
				return EditorCommandResult::Ok({ { "path", created } });
			} });

		registry.Register({ "asset.moveFolder",
			"Moves or renames a folder of the asset directory with everything in it (files that are no assets too, such as a model's buffers); every "
			"asset in it keeps its handle, so references stay valid. One undo step. Fails when the destination exists. Returns {path (the new one), "
			"assets (how many assets moved)}.",
			ObjectSchema({ { "path", StringSchema("Folder to move, relative to the asset directory") },
				{ "to", StringSchema("Its new path relative to the asset directory, e.g. \"Art/Materials\"") } }, { "path", "to" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const std::string from = NormalizeFolderPath(arguments.GetString("path"));
				const std::string to = NormalizeFolderPath(arguments.GetString("to"));
				if (arguments.IsValid() && !assets->HasFolder(from))
					arguments.SetError(fmt::format("Parameter 'path': '{}' is no folder of the asset directory", from));
				if (!arguments.IsValid())
					return arguments.Fail();
				const size_t count = assets->GetAssetsBelowFolder(from).size();
				if (!assets->MoveFolder(from, to, &error))
					return EditorCommandResult::Fail(error);
				if (to != from)
					context.GetUndoStack().Record(CreateScope<AssetMoveAction>(*assets, fmt::format("Move Folder {}", from), UUID::Null(), from, to));
				return EditorCommandResult::Ok({ { "path", to }, { "assets", count } });
			} });

		registry.Register({ "asset.deleteFolder",
			"Deletes a folder with everything in it: moves it to the project's trash (.strata/Trash) and removes its assets from the project, in one undo "
			"step (edit.undo restores it, every asset with its handle, as long as the edited scene stays; the trash keeps it for 30 days). Fails while "
			"assets outside the folder or the open scene use an asset in it, "
			"listing them in the error's data (referencedBy); force: true deletes it anyway. Fails when it holds the open scene. Returns {path, assets, "
			"trash, referencedBy}.",
			ObjectSchema({ { "path", StringSchema("Folder relative to the asset directory") },
				{ "force", BoolSchema("Delete although assets outside it or the open scene use its assets (default false)") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const std::string path = NormalizeFolderPath(arguments.GetString("path"));
				const bool force = arguments.GetBool("force", false);
				if (arguments.IsValid() && (path.empty() || !assets->HasFolder(path)))
					arguments.SetError(fmt::format("Parameter 'path': '{}' is no folder of the asset directory", path));
				if (!arguments.IsValid())
					return arguments.Fail();

				const std::vector<AssetHandle> contained = assets->GetAssetsBelowFolder(path);
				if (std::find(contained.begin(), contained.end(), context.GetSceneHandle()) != contained.end())
					return EditorCommandResult::Fail(fmt::format("'{}' holds the open scene, which cannot be deleted; open another scene first", path));
				const std::vector<AssetHandle> targets = AssetReferences::WithSubAssets(context, contained);
				const std::vector<AssetReferencer> referencers = AssetReferences::Find(context, targets, { targets.begin(), targets.end() });
				if (!referencers.empty() && !force)
				{
					return EditorCommandResult::Fail(fmt::format("Assets in '{}' are used by {}; delete it with force: true to leave those references "
						"missing, or change them first", path, AssetReferences::Describe(referencers)),
						{ { "referencedBy", AssetReferences::ToJson(referencers) } });
				}
				TrashedAssets trashed;
				if (!assets->MoveToTrash(path, trashed, &error))
					return EditorCommandResult::Fail(error);
				context.GetUndoStack().Record(CreateScope<AssetTrashAction>(context, *assets, fmt::format("Delete Folder {}", path), trashed, force));
				context.NotifyAssetsTrashed(path, true, trashed.Handles.size(), !referencers.empty());
				return EditorCommandResult::Ok({ { "path", path }, { "assets", trashed.Handles.size() }, { "trash", FileSystem::ToUTF8(trashed.Entry) },
					{ "referencedBy", AssetReferences::ToJson(referencers) } });
			} });

		registry.Register({ "asset.reimport",
			"Imports an asset again from its source file, in the background (the editor keeps running and the asset keeps its previous version until "
			"the new one is loaded). Returns when the import is done (its settings, warnings) and fails when it failed; wait: false returns at once "
			"(asset.info tells when it is done).",
			ObjectSchema({ { "asset", AssetSchema("Asset") }, { "wait", BoolSchema("Return when the import is done (default true)") } }, { "asset" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				const bool wait = arguments.GetBool("wait", true);
				if (arguments.IsValid() && !CheckSourceAsset(*assets, handle, &error))
					arguments.SetError(fmt::format("Parameter 'asset': {}", error));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!assets->ReimportAssetInBackground(handle, &error))
					return EditorCommandResult::Fail(error);
				return FinishImport(*assets, handle, wait);
			} });

		registry.Register({ "asset.setImportSettings",
			"Changes import settings (merged into the current ones; asset.info lists every setting with its type, range or options), saves the .meta "
			"and imports the asset again in the background, without blocking the editor. Returns when the import is done (settings, warnings) and "
			"fails when it failed; wait: false returns at once. Unknown settings, invalid values and numbers outside a setting's range change "
			"nothing (they are refused, never clamped).",
			ObjectSchema({
				{ "asset", AssetSchema("Asset") },
				{ "settings", AnyObjectSchema("Settings to change, e.g. {\"MaxSize\": 1024} for a texture or {\"Scale\": 0.01} for a model") },
				{ "wait", BoolSchema("Return when the import is done (default true)") } }, { "asset", "settings" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				const nlohmann::json& settings = arguments.GetObject("settings");
				const bool wait = arguments.GetBool("wait", true);
				if (arguments.IsValid() && !CheckSourceAsset(*assets, handle, &error))
					arguments.SetError(fmt::format("Parameter 'asset': {}", error));
				const AssetImporter* importer = arguments.IsValid() ? assets->GetImporter(handle) : nullptr;
				nlohmann::json normalized;
				if (arguments.IsValid() && (!importer || !NormalizeImportSettings(*importer, settings, normalized, &error)))
					arguments.SetError(fmt::format("Parameter 'settings': {}", importer ? error : std::string("the asset has no importer")));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!assets->SetImportSettingsInBackground(handle, normalized, &error))
					return EditorCommandResult::Fail(error);
				return FinishImport(*assets, handle, wait);
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Materials and prefabs
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "material.get",
			"A material asset's property values, and every material property with its type, range or options: what material.create and "
			"material.set accept. \"saved\" is false while values previewed with material.set {save: false} are not saved.",
			ObjectSchema({ { "material", AssetSchema("Material: handle, path or Builtin/DefaultMaterial") } }, { "material" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "material", AssetType::Material);
				if (!arguments.IsValid())
					return arguments.Fail();
				Ref<Material> material = AssetManager::LoadAssetSync<Material>(handle);
				EditorAssetManager* assets = context.GetAssetManager();
				if (!material)
					return EditorCommandResult::Fail(fmt::format("Loading the material failed: {}", assets ? assets->GetAssetError(handle) : "not loadable"));
				nlohmann::json properties = nlohmann::json::array();
				for (const PropertyInfo& property : Material::GetPropertyInfos())
					properties.push_back(DescribeProperty(property));
				nlohmann::json values = material->Serialize()["Material"];
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "values", std::move(values) }, { "properties", std::move(properties) },
					{ "saved", !assets || AssetEdit::IsMaterialSaved(*assets, handle) } });
			}, EditorCommandKind::Query, EditorCommandResults::Content, true });

		registry.Register({ "material.create", "Creates a material asset (.stmat) from the default material with the given property values.",
			ObjectSchema({
				{ "path", StringSchema("Path relative to the asset directory, e.g. \"Materials/Red.stmat\"") },
				{ "properties", AnyObjectSchema(fmt::format("Property values, e.g. {{\"BaseColor\": [1, 0, 0, 1], \"Roughness\": 0.3, \"BaseColorMap\": "
					"\"Textures/Brick.png\"}}; texture maps take a texture by path or handle, null clears them. Properties: {} (material.get describes "
					"their types)", AssetEdit::ListMaterialProperties())) } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				const nlohmann::json* properties = arguments.FindObject("properties");
				if (!arguments.IsValid())
					return arguments.Fail();
				EditorCommandError kind = EditorCommandError::Failed;
				const std::optional<std::string> document = AssetEdit::BuildMaterialDocument(Material::Create()->Serialize(), properties, &error, &kind);
				if (!document)
					return EditorCommandResult::Fail(error, kind);
				const AssetHandle handle = assets->CreateNativeAsset(path, AsBytes(*document), &error);
				if (!handle.IsValid())
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) } });
			} });

		registry.Register({ "material.set",
			"Changes property values of a material asset of the project; entities using it show the change in the same frame. By default it saves the "
			"material (one undo step, edit.undo restores the file and the loaded values); save: false only previews the values in the loaded material "
			"(no file is written, no undo step), e.g. while a value is dragged, until a saving material.set (which saves them too), an undo or a "
			"reload replaces them. Returns the material's values.",
			ObjectSchema({
				{ "material", AssetSchema("Material") },
				{ "properties", AnyObjectSchema(fmt::format("Property values to change, e.g. {{\"BaseColor\": [1, 0, 0, 1]}}. Properties: {} (material.get "
					"describes their types)", AssetEdit::ListMaterialProperties())) },
				{ "save", BoolSchema("Save the material and record an undo step (default true); false previews the values without saving") } },
				{ "material", "properties" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				if (!RequireAssets(context, &error))
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "material", AssetType::Material);
				const nlohmann::json& properties = arguments.GetObject("properties");
				const bool save = arguments.GetBool("save", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				AssetEdit::MaterialEdit edit;
				edit.Preview = !save;
				EditorCommandError kind = EditorCommandError::Failed;
				if (!AssetEdit::SetMaterialProperties(context, handle, properties, edit, &error, &kind))
					return EditorCommandResult::Fail(error, kind);
				Ref<Material> material = AssetManager::LoadAssetSync<Material>(handle);
				if (!material)
					return EditorCommandResult::Fail(fmt::format("Loading the material failed: {}", context.GetAssetManager()->GetAssetError(handle)));
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "values", material->Serialize()["Material"] }, { "saved", save } });
			} });

		registry.Register({ "prefab.create",
			"Saves entities and their descendants as a new prefab asset (.stprefab). With link (the default) they become an instance of it, linked as "
			"prefab.instantiate's instances are (prefab.apply, prefab.revert, prefab.status); one undo step for the file and the link (edit.undo moves "
			"the file to the trash and unlinks them). Links of another prefab they had are replaced. Returns {asset, path, linked}.",
			ObjectSchema({ { "entities", EntityArraySchema("Root entities of the prefab") }, { "path", StringSchema("Path relative to the asset directory") },
				{ "link", BoolSchema("Make the entities an instance of the new prefab (default true; not while playing; refused while a scene is being "
					"opened)") } }, { "entities", "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = arguments.GetEntities(scene, "entities");
				const std::string path = arguments.GetString("path");
				const bool link = arguments.GetBool("link", true) && !context.IsPlaying();
				if (!arguments.IsValid())
					return arguments.Fail();
				// Linking changes the scene, recorded outside CommitEdit: refused while a scene is being opened (without the link
				// the entities are only read).
				if (link && !context.CheckSceneEditable(&error))
					return EditorCommandResult::Fail(fmt::format("{} (prefab.create with link: false only writes the prefab)", error));
				Ref<Prefab> prefab = Prefab::CreateFromEntities(scene, entities);
				if (!prefab)
					return EditorCommandResult::Fail("Capturing the entities failed");
				// A prefab stores no links: they belong to the instances in scenes (WriteDocument leaves them out, and so does the
				// snapshot the instances are marked in sync with).
				StringDocumentOutput output;
				if (!prefab->WriteDocument(output, &error))
					return EditorCommandResult::Fail(fmt::format("Writing the prefab failed: {}", error));
				const std::string text = output.TakeText();
				nlohmann::json snapshot = prefab->GetSnapshot();
				PrefabInstances::StripLinks(snapshot);
				const AssetHandle handle = assets->CreateNativeAsset(path, AsBytes(text), &error);
				if (!handle.IsValid())
					return EditorCommandResult::Fail(error);
				const std::string created = assets->GetMetadata(handle)->Path;

				// One step: the file (undo moves it to the trash) and the link.
				Scope<EditorActionGroup> step = CreateScope<EditorActionGroup>(fmt::format("Create Prefab {}", created));
				step->Add(CreateScope<AssetCreateAction>(*assets, fmt::format("Create Prefab {}", created), handle, created));
				if (link)
				{
					SceneEditTransaction transaction(scene, "Link Prefab", {});
					for (const Entity entity : entities)
						transaction.TrackSubtree(entity.GetUUID());
					PrefabInstances::Link(scene, entities, handle);
					for (const Entity entity : entities)
						PrefabInstances::MarkSynced(scene, entity, snapshot);
					step->Add(transaction.TakeAction());
				}
				if (!context.IsPlaying())
					context.GetUndoStack().Record(std::move(step));
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) }, { "path", created }, { "linked", link } });
			} });
	}

}
