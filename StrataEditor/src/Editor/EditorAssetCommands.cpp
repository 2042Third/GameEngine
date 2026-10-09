#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/GameExport.h"

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

		// A material document from the defaults with the given property values applied; validated by the loader.
		std::optional<std::string> BuildMaterialDocument(const nlohmann::json& base, const nlohmann::json* properties, std::string* outError)
		{
			nlohmann::json document = base;
			if (properties)
			{
				for (const auto& [name, value] : properties->items())
				{
					const auto& infos = Material::GetPropertyInfos();
					const bool known = std::any_of(infos.begin(), infos.end(), [&](const PropertyInfo& property) { return property.Name == name; });
					if (!known)
					{
						if (outError)
							*outError = fmt::format("Unknown material property '{}'", name);
						return std::nullopt;
					}
					document["Material"][name] = value;
				}
			}
			std::vector<std::string> warnings;
			Ref<Material> material = Material::FromJson(document, outError, &warnings);
			if (!material)
				return std::nullopt;
			if (!warnings.empty())
			{
				if (outError)
					*outError = warnings.front();
				return std::nullopt;
			}
			return JsonUtils::Dump(material->Serialize(), 1, '\t') + "\n";
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

		registry.Register({ "project.create", "Creates a project in a directory and opens it.",
			ObjectSchema({ { "directory", StringSchema("Absolute directory for the project") }, { "name", StringSchema("Project name") } }, { "directory", "name" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string directory = arguments.GetString("directory");
				const std::string name = arguments.GetString("name");
				if (!arguments.IsValid())
					return arguments.Fail();
				std::string error;
				if (!context.CreateProject(FileSystem::FromUTF8(directory), name, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "projectFile", FileSystem::ToUTF8(context.GetProject()->GetProjectFile()) } });
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

		registry.Register({ "project.export", "Exports the project as a playable game (runtime executable, asset pack, script module, manifest) into a directory outside the project. The game runs the script module the editor has loaded (build it first with script.build).",
			ObjectSchema({
				{ "directory", StringSchema("Absolute output directory") },
				{ "includeRuntime", BoolSchema("Copy the runtime executable (default true)") },
				{ "includeScriptSymbols", BoolSchema("Copy the script module's debug symbols (PDB) too (default: true, false in Dist builds)") },
				{ "runtime", StringSchema("Runtime executable to copy (default: StrataRuntime next to the editor)") },
				{ "width", IntegerSchema("Window width (default 1280)", 1, 16384) },
				{ "height", IntegerSchema("Window height (default 720)", 1, 16384) },
				{ "fullscreen", BoolSchema("Start in fullscreen (default false)") } }, { "directory" }),
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
				if (!arguments.IsValid())
					return arguments.Fail();
				GameExportResult result;
				std::string error;
				if (!ExportGame(context, options, result, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({
					{ "executable", result.Executable.empty() ? nlohmann::json(nullptr) : nlohmann::json(FileSystem::ToUTF8(result.Executable)) },
					{ "manifest", FileSystem::ToUTF8(result.Manifest) },
					{ "assetPack", FileSystem::ToUTF8(result.AssetPack) },
					{ "scriptModule", result.ScriptModule.empty() ? nlohmann::json(nullptr) : nlohmann::json(FileSystem::ToUTF8(result.ScriptModule)) },
					{ "assetCount", result.AssetCount } });
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

		registry.Register({ "asset.info", "An asset's metadata, load state, import errors and warnings, sub-assets and import settings.",
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

		registry.Register({ "asset.reimport", "Imports an asset again from its source file.",
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
				if (!assets->ReimportAsset(handle, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok();
			} });

		registry.Register({ "asset.setImportSettings", "Changes import settings (merged into the current ones), saves the .meta and imports again.",
			ObjectSchema({ { "asset", AssetSchema("Asset") }, { "settings", AnyObjectSchema("Settings to change, e.g. {\"Scale\": 0.01}") } }, { "asset", "settings" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "asset", AssetType::None);
				const nlohmann::json& settings = arguments.GetObject("settings");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!assets->SetImportSettings(handle, settings, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "importSettings", assets->GetImportSettings(handle) } });
			} });

		////////////////////////////////////////////////////////////////////////////////
		// Materials and prefabs
		////////////////////////////////////////////////////////////////////////////////

		registry.Register({ "material.create", "Creates a material asset (.stmat) from the default material with the given property values.",
			ObjectSchema({
				{ "path", StringSchema("Path relative to the asset directory, e.g. \"Materials/Red.stmat\"") },
				{ "properties", AnyObjectSchema("Property values, e.g. {\"BaseColor\": [1, 0, 0, 1], \"Roughness\": 0.3}") } }, { "path" }),
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
				const std::optional<std::string> document = BuildMaterialDocument(Material::Create()->Serialize(), properties, &error);
				if (!document)
					return EditorCommandResult::Fail(error);
				const AssetHandle handle = assets->CreateNativeAsset(path, AsBytes(*document), &error);
				if (!handle.IsValid())
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "asset", UUIDToJson(handle) } });
			} });

		registry.Register({ "material.set", "Changes property values of a material asset and saves it; entities using it update.",
			ObjectSchema({ { "material", AssetSchema("Material") }, { "properties", AnyObjectSchema("Property values to change") } }, { "material", "properties" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				std::string error;
				EditorAssetManager* assets = RequireAssets(context, &error);
				if (!assets)
					return EditorCommandResult::Fail(error);
				CommandArguments arguments(parameters);
				const AssetHandle handle = ResolveAsset(context, arguments, "material", AssetType::Material);
				const nlohmann::json& properties = arguments.GetObject("properties");
				if (!arguments.IsValid())
					return arguments.Fail();
				Ref<Material> current = AssetManager::LoadAssetSync<Material>(handle);
				if (!current)
					return EditorCommandResult::Fail(fmt::format("Loading the material failed: {}", assets->GetAssetError(handle)));
				const std::optional<std::string> document = BuildMaterialDocument(current->Serialize(), &properties, &error);
				if (!document)
					return EditorCommandResult::Fail(error);
				if (!assets->SaveNativeAsset(handle, AsBytes(*document), true, &error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok();
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
