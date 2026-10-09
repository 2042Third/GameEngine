#include "Editor/GameExport.h"

#include "Editor/EditorContext.h"

#include <Strata/Asset/AssetPack.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Project/GameManifest.h>

#include <cctype>

namespace Strata
{

	namespace
	{

		constexpr const char* c_NoticesFile = "ThirdPartyNotices.md";

		// A name usable as a file name on every platform: letters, digits, spaces, '-' and '_'.
		std::string ToFileName(const std::string& name)
		{
			std::string result;
			for (char character : name)
			{
				const unsigned char byte = static_cast<unsigned char>(character);
				result += std::isalnum(byte) || character == ' ' || character == '-' || character == '_' ? character : '_';
			}
			while (!result.empty() && (result.back() == ' ' || result.back() == '.'))
				result.pop_back();
			return result.empty() ? std::string("Game") : result;
		}

		// True if an entity snapshot ({"Entities": [...]}) attaches at least one script.
		bool UsesScripts(const nlohmann::json& snapshot)
		{
			const nlohmann::json* entities = JsonUtils::Find(snapshot, "Entities");
			if (!entities || !entities->is_array())
				return false;
			for (const nlohmann::json& entity : *entities)
			{
				const nlohmann::json* components = JsonUtils::Find(entity, "Components");
				const nlohmann::json* script = components ? JsonUtils::Find(*components, "Script") : nullptr;
				const nlohmann::json* scripts = script ? JsonUtils::Find(*script, "Scripts") : nullptr;
				if (scripts && scripts->is_array() && !scripts->empty())
					return true;
			}
			return false;
		}

		// The first scene or prefab of the project that attaches scripts (empty when none does). The saved files are read,
		// as the export packs them; loaded copies may be older.
		std::string FindAssetUsingScripts(EditorAssetManager& assets)
		{
			for (const AssetMetadata& metadata : assets.GetAllMetadata())
			{
				const char* documentKey = metadata.Type == AssetType::Scene ? "Scene" : metadata.Type == AssetType::Prefab ? "Prefab" : nullptr;
				if (!documentKey || metadata.IsSubAsset())
					continue;
				const std::optional<std::string> text = FileSystem::ReadText(assets.GetAbsolutePath(metadata.Handle));
				const std::optional<nlohmann::json> document = text ? JsonUtils::Parse(*text) : std::nullopt;
				const nlohmann::json* content = document ? JsonUtils::Find(*document, documentKey) : nullptr;
				if (content && UsesScripts(*content))
					return metadata.Path;
			}
			return {};
		}

	}

	std::string GetExecutableFileName(const std::string& name)
	{
#if defined(ST_PLATFORM_WINDOWS)
		return name + ".exe";
#else
		return name;
#endif
	}

	bool ExportGame(EditorContext& context, const GameExportOptions& options, GameExportResult& outResult, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		EditorAssetManager* assets = context.GetAssetManager();
		if (!assets)
			return fail("No project is open");
		if (context.IsPlaying())
			return fail("Stop playing before exporting");
		if (context.IsSceneModified())
			return fail("The open scene has unsaved changes; save it first (the export uses the saved files)");
		if (options.Directory.empty() || !options.Directory.is_absolute())
			return fail("The export directory must be an absolute path");
		const Project& project = *context.GetProject();
		if (FileSystem::IsInside(options.Directory, project.GetProjectDirectory()))
			return fail("Export outside the project directory (the export would become part of the project)");

		AssetHandle startScene = project.GetConfig().StartScene;
		if (!startScene.IsValid())
			startScene = context.GetSceneHandle();
		if (!startScene.IsValid() || assets->GetAssetType(startScene) != AssetType::Scene)
			return fail("The project has no start scene: save the scene and set it with project.setStartScene");

		std::filesystem::path runtime = options.RuntimeExecutable;
		if (options.IncludeRuntime)
		{
			if (runtime.empty())
				runtime = Platform::GetExecutableDirectory() / GetExecutableFileName("StrataRuntime");
			if (!FileSystem::IsRegularFile(runtime))
				return fail(fmt::format("The runtime executable '{}' is missing (build the StrataRuntime target)", FileSystem::ToUTF8(runtime)));
			if (!FileSystem::IsRegularFile(runtime.parent_path() / c_NoticesFile))
				return fail(fmt::format("'{}' is missing next to the runtime; games must ship the third-party notices", c_NoticesFile));
		}

		// The game runs the scripts the editor runs: the loaded module, exactly as it was loaded.
		ScriptModuleFile scriptModule;
		std::string scriptError;
		if (!context.ReadRunningScriptModule(scriptModule, &scriptError))
			return fail(scriptError);
		if (scriptModule.Path.empty())
		{
			const std::string user = FindAssetUsingScripts(*assets);
			if (!user.empty())
			{
				return fail(fmt::format("'{}' uses scripts, but no script module is loaded: build the scripts (script.build) or load the built module "
					"(script.reload)", user));
			}
		}

		if (!FileSystem::CreateDirectories(options.Directory))
			return fail(fmt::format("Could not create '{}'", FileSystem::ToUTF8(options.Directory)));

		const std::string name = ToFileName(project.GetConfig().Name);
		GameExportResult result;
		result.AssetPack = options.Directory / FileSystem::FromUTF8(name + ".stpak");
		std::string error;
		if (!assets->BuildAssetPack(result.AssetPack, &error))
			return fail(fmt::format("Building the asset pack failed: {}", error));
		result.AssetCount = assets->GetAllMetadata().size();

		GameManifest manifest;
		manifest.Name = project.GetConfig().Name;
		manifest.AssetPack = FileSystem::ToUTF8(result.AssetPack.filename());
		manifest.StartScene = startScene;
		if (!scriptModule.Path.empty())
		{
			result.ScriptModule = options.Directory / scriptModule.Path.filename();
			if (!FileSystem::WriteBytes(result.ScriptModule, scriptModule.Bytes))
				return fail(fmt::format("Could not write the script module to '{}'", FileSystem::ToUTF8(result.ScriptModule)));
			std::filesystem::path symbols = scriptModule.Path;
			symbols.replace_extension(".pdb");
			if (options.IncludeScriptSymbols && FileSystem::IsRegularFile(symbols)
				&& !FileSystem::Copy(symbols, options.Directory / symbols.filename(), true))
			{
				return fail(fmt::format("Could not copy the script module's symbols '{}'", FileSystem::ToUTF8(symbols)));
			}
			manifest.ScriptModule = FileSystem::ToUTF8(result.ScriptModule.filename());
		}
		manifest.WindowWidth = options.WindowWidth;
		manifest.WindowHeight = options.WindowHeight;
		manifest.Fullscreen = options.Fullscreen;
		result.Manifest = options.Directory / FileSystem::FromUTF8(name + std::string(GameManifest::c_FileExtension));
		if (!manifest.Save(result.Manifest, outError))
			return false;

		if (options.IncludeRuntime)
		{
			result.Executable = options.Directory / FileSystem::FromUTF8(GetExecutableFileName(name));
			if (!FileSystem::Copy(runtime, result.Executable, true))
				return fail(fmt::format("Could not copy the runtime to '{}'", FileSystem::ToUTF8(result.Executable)));
			if (!FileSystem::Copy(runtime.parent_path() / c_NoticesFile, options.Directory / c_NoticesFile, true))
				return fail(fmt::format("Could not copy '{}'", c_NoticesFile));
#if !defined(ST_PLATFORM_WINDOWS)
			std::error_code permissionError;
			std::filesystem::permissions(result.Executable, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
				std::filesystem::perm_options::add, permissionError);
#endif
		}

		ST_INFO("Exported '{}' to {}", manifest.Name, FileSystem::ToUTF8(options.Directory));
		outResult = std::move(result);
		return true;
	}

}
