#include "Editor/AssetEdit.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameExport.h"
#include "Editor/ProjectSamples.h"
#include "Editor/ProjectTemplates.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Scene/Prefab.h>

#include <algorithm>

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
			} });

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
			} });

		registry.Register({ "project.create",
			"Creates a project in a directory and opens it. With template \"basic3d\" it starts with a saved, lit start scene (Scenes/Main.stscene: Main "
			"Camera, Sun, Sky, Ground, Post Process); the default \"empty\" has no scene (see project.templates).",
			ObjectSchema({
				{ "directory", StringSchema("Absolute directory for the project") },
				{ "name", StringSchema("Project name") },
				{ "template", { { "type", "string" }, { "enum", { ProjectTemplates::c_Empty, ProjectTemplates::c_Basic3D } },
					{ "description", "What the project starts with (default \"empty\"; see project.templates)" } } } }, { "directory", "name" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string directory = arguments.GetString("directory");
				const std::string name = arguments.GetString("name");
				const std::string templateId = arguments.GetString("template", std::string(ProjectTemplates::c_Empty));
				if (arguments.IsValid() && !ProjectTemplates::Find(templateId))
					arguments.SetError(fmt::format("Unknown template '{}' (templates: {})", templateId, ProjectTemplates::ListIds()));
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.CreateProject(FileSystem::FromUTF8(directory), name, templateId, &error))
					return EditorCommandResult::Fail(error);
				const AssetHandle startScene = context.GetProject()->GetConfig().StartScene;
				return EditorCommandResult::Ok({
					{ "projectFile", FileSystem::ToUTF8(context.GetProject()->GetProjectFile()) },
					{ "template", templateId },
					{ "startScene", startScene.IsValid() ? UUIDToJson(startScene) : nlohmann::json(nullptr) } });
			} });

		registry.Register({ "project.open", "Opens a project file or the project in a directory (unsaved scene changes are discarded).",
			ObjectSchema({ { "path", StringSchema("Project file (.stproj) or project directory") } }, { "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.OpenProject(FileSystem::FromUTF8(path), &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok();
			} });

		registry.Register({ "project.close",
			"Closes the open project (unsaved scene changes are discarded); the editor shows its launcher. Without a project it does nothing.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				const bool hadProject = context.HasProject();
				context.CloseProject();
				return EditorCommandResult::Ok({ { "closed", hadProject } });
			} });

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
			} });

		registry.Register({ "project.openSample",
			"Copies a sample project (project.samples) into a new directory and opens the copy, like project.open (unsaved scene changes are "
			"discarded); the sample itself stays unchanged. The copy leaves out local editor data (.strata): build its scripts with script.build "
			"before playing.",
			ObjectSchema({
				{ "sample", StringSchema("The sample's id from project.samples, e.g. \"Tetris\"") },
				{ "directory", StringSchema("Absolute directory for the copy: created, or an existing empty directory") } }, { "sample", "directory" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string id = arguments.GetString("sample");
				const std::string directory = arguments.GetString("directory");
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
				const std::filesystem::path projectFile = ProjectSamples::Copy(*sample, FileSystem::FromUTF8(directory), &error);
				if (projectFile.empty())
					return EditorCommandResult::Fail(error);
				if (!context.OpenProject(projectFile, &error))
					return EditorCommandResult::Fail(fmt::format("The sample was copied to '{}' but could not be opened: {}", directory, error));
				return EditorCommandResult::Ok({ { "sample", sample->Id }, { "projectFile", FileSystem::ToUTF8(projectFile) } });
			} });

		registry.Register({ "project.setStartScene", "Sets the scene a built game starts with and saves the project file.",
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
				project.GetConfig().StartScene = scene;
				if (!project.Save(&error))
				{
					project.GetConfig().StartScene = previous;
					return EditorCommandResult::Fail(error);
				}
				return EditorCommandResult::Ok();
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
				return EditorCommandResult::Defer([exporter, firstMeasured, frames = uint64_t(0), lastRecorded = uint64_t(0), frameTimes = std::vector<double>()](
					EditorContext& context) mutable -> std::optional<EditorCommandResult>
				{
					frames++;
					const uint64_t recorded = context.GetRecordedFrameCount();
					if (recorded >= firstMeasured && recorded != lastRecorded)
					{
						frameTimes.push_back(context.GetLastFrameTime());
						lastRecorded = recorded;
					}
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
					if (!frameTimes.empty())
						value["frameTimes"] = DescribeFrameTimes(std::move(frameTimes));
					return EditorCommandResult::Ok(std::move(value));
				});
			} });

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
			} });

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
			} });

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
			} });

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
			} });

		registry.Register({ "asset.import", "Copies an external file into the project and imports it. Returns the new asset.",
			ObjectSchema({ { "file", StringSchema("Absolute path of the file to import") }, { "directory", StringSchema("Target directory relative to the asset directory (default: the root)") } },
				{ "file" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const std::string file = arguments.GetString("file");
				const std::string directory = arguments.GetString("directory", "");
				if (!arguments.IsValid())
					return arguments.Fail();
				const AssetHandle handle = assets->ImportExternalFile(FileSystem::FromUTF8(file), directory, &error);
				if (!handle.IsValid())
					return EditorCommandResult::Fail(error);
				nlohmann::json result = DescribeAsset(*assets->GetMetadata(handle));
				result["importError"] = assets->GetImportInfo(handle).Error;
				return EditorCommandResult::Ok(std::move(result));
			} });

		registry.Register({ "asset.move", "Moves or renames an asset (with its .meta); its handle and all references stay valid.",
			ObjectSchema({ { "asset", AssetSchema("Asset to move") }, { "path", StringSchema("New path relative to the asset directory") } }, { "asset", "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!assets->MoveAsset(handle, path, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok();
			} });

		registry.Register({ "asset.delete", "Deletes an asset's file, .meta and cached data. References to it become missing.",
			ObjectSchema({ { "asset", AssetSchema("Asset to delete") } }, { "asset" }),
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
				if (handle == context.GetSceneHandle())
					return EditorCommandResult::Fail("The open scene cannot be deleted; open another scene first");
				if (!assets->DeleteAsset(handle, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok();
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
			} });

		registry.Register({ "material.create", "Creates a material asset (.stmat) from the default material with the given property values.",
			ObjectSchema({
				{ "path", StringSchema("Path relative to the asset directory, e.g. \"Materials/Red.stmat\"") },
				{ "properties", AnyObjectSchema(fmt::format("Property values, e.g. {{\"BaseColor\": [1, 0, 0, 1], \"Roughness\": 0.3}}. Properties: {} "
					"(material.get describes their types)", AssetEdit::ListMaterialProperties())) } }, { "path" }),
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

		registry.Register({ "prefab.create", "Saves entities and their descendants as a new prefab asset (.stprefab).",
			ObjectSchema({ { "entities", EntityArraySchema("Root entities of the prefab") }, { "path", StringSchema("Path relative to the asset directory") } },
				{ "entities", "path" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const std::vector<Entity> entities = arguments.GetEntities(*context.GetActiveScene(), "entities");
				const std::string path = arguments.GetString("path");
				if (!arguments.IsValid())
					return arguments.Fail();
				Ref<Prefab> prefab = Prefab::CreateFromEntities(*context.GetActiveScene(), entities);
				if (!prefab)
					return EditorCommandResult::Fail("Capturing the entities failed");
				const std::string document = JsonUtils::Dump(prefab->Serialize(), 1, '\t') + "\n";
				const AssetHandle handle = assets->CreateNativeAsset(path, AsBytes(document), &error);
				if (!handle.IsValid())
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) } });
			} });
	}

}
