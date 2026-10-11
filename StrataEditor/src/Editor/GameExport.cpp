#include "Editor/GameExport.h"

#include "Editor/EditorContext.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/StringUtils.h>

#include <algorithm>
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

		// The first scene or prefab the pack holds that attaches scripts (empty when none does). The saved files are read, as
		// the export packs them (loaded copies may be older), through the manager: under its file lock, so a save of the same
		// document meanwhile still replaces its file. Any thread.
		std::string FindAssetUsingScripts(EditorAssetManager& assets, const AssetPackBuildPlan& plan)
		{
			std::vector<uint8_t> bytes;
			for (const AssetPackSource& source : plan.Assets)
			{
				const AssetMetadata& metadata = source.Metadata;
				const char* documentKey = metadata.Type == AssetType::Scene ? "Scene" : metadata.Type == AssetType::Prefab ? "Prefab" : nullptr;
				if (!documentKey || metadata.IsSubAsset())
					continue;
				bytes.clear();
				const std::optional<std::string> text = assets.ReadStoredData(metadata.Handle, bytes) ? std::optional<std::string>(std::string(bytes.begin(), bytes.end()))
					: std::nullopt;
				const std::optional<nlohmann::json> document = text ? JsonUtils::Parse(*text) : std::nullopt;
				const nlohmann::json* content = document ? JsonUtils::Find(*document, documentKey) : nullptr;
				if (content && UsesScripts(*content))
					return metadata.Path;
			}
			return {};
		}

		// A file the export could not write, with what may be the cause on Windows: a path over its classic length limit.
		std::string DescribeWriteFailure(std::string_view what, const std::filesystem::path& path)
		{
			std::string message = fmt::format("Could not write {} '{}'", what, FileSystem::ToUTF8(path));
#if defined(ST_PLATFORM_WINDOWS)
			constexpr size_t c_ClassicMaxPath = 259;
			if (path.native().size() > c_ClassicMaxPath)
			{
				message += fmt::format(" (its path has {} characters: Windows refuses paths over {} unless long paths are enabled; choose a shorter "
					"directory)", path.native().size(), c_ClassicMaxPath);
			}
#endif
			return message;
		}

	}

	const char* GameExportStateToString(GameExportState state)
	{
		switch (state)
		{
			case GameExportState::WaitingForImports: return "WaitingForImports";
			case GameExportState::Packing:           return "Packing";
			case GameExportState::Copying:           return "Copying";
			case GameExportState::Succeeded:         return "Succeeded";
			case GameExportState::Failed:            return "Failed";
			case GameExportState::Cancelled:         return "Cancelled";
		}
		return "Unknown";
	}

	std::string GetExecutableFileName(const std::string& name)
	{
#if defined(ST_PLATFORM_WINDOWS)
		return name + ".exe";
#else
		return name;
#endif
	}

	std::optional<std::string> CheckExportDirectory(const std::filesystem::path& directory, const std::filesystem::path& projectDirectory)
	{
		const std::string text = FileSystem::ToUTF8(directory);
		if (text.empty())
			return "Choose the directory to export the game to";
		// "\\server\share", "//server" and the "\\?\" and "\\.\" device namespaces all start with two separators.
		auto isSeparator = [](char character) { return character == '/' || character == '\\'; };
		if (text.size() >= 2 && isSeparator(text[0]) && isSeparator(text[1]))
			return fmt::format("'{}' is a network or device path: export to a folder on a local drive", text);
		if (!directory.is_absolute())
			return fmt::format("The export directory must be an absolute path, such as {} (got '{}')",
				FileSystem::ToUTF8(projectDirectory.parent_path() / (FileSystem::ToUTF8(projectDirectory.filename()) + " Build")), text);
		// Windows opens devices for these names in any folder and with any extension.
		for (const std::filesystem::path& part : directory.relative_path())
		{
			std::string name = StringUtils::ToLower(FileSystem::ToUTF8(part));
			name = name.substr(0, name.find('.'));
			const bool numbered = name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3] >= '0' && name[3] <= '9';
			if (name == "con" || name == "prn" || name == "aux" || name == "nul" || name == "conin$" || name == "conout$" || numbered)
				return fmt::format("'{}' in '{}' is a reserved device name on Windows: choose another folder name", FileSystem::ToUTF8(part), text);
		}
		if (FileSystem::IsInside(directory, projectDirectory))
		{
			return fmt::format("'{}' lies inside the project directory '{}': export outside the project (the export would become part of the "
				"project)", text, FileSystem::ToUTF8(projectDirectory));
		}
		std::error_code error;
		const std::filesystem::file_status status = std::filesystem::status(directory, error);
		if (!error && std::filesystem::exists(status) && !std::filesystem::is_directory(status))
			return fmt::format("'{}' is a file, not a directory: choose a directory to export into", text);
		return std::nullopt;
	}

	Ref<GameExport> GameExport::Start(EditorContext& context, const GameExportOptions& options, std::string* outError)
	{
		auto fail = [outError](std::string message) -> Ref<GameExport>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};

		EditorAssetManager* assets = context.GetAssetManager();
		if (!assets)
			return fail("No project is open");
		if (context.IsPlaying())
			return fail("Stop playing before exporting");
		if (context.IsSceneModified())
			return fail("The open scene has unsaved changes; save it first (the export uses the saved files)");
		const Project& project = *context.GetProject();
		if (std::optional<std::string> problem = CheckExportDirectory(options.Directory, project.GetProjectDirectory()))
			return fail(std::move(*problem));

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

		// The game runs the scripts the editor runs: the loaded module, exactly as it was loaded. Without one, the I/O job
		// checks that no packed scene or prefab uses scripts (reading them could take frames).
		ScriptModuleFile scriptModule;
		std::string scriptError;
		if (!context.ReadRunningScriptModule(scriptModule, &scriptError))
			return fail(scriptError);

		// The scene the game plays while its start scene loads (optional); a pruned pack keeps it with what it uses.
		const AssetHandle loadingScene = project.GetConfig().Export.LoadingScene;
		std::optional<AssetMetadata> loadingSceneMetadata;
		if (loadingScene.IsValid())
		{
			loadingSceneMetadata = assets->GetMetadata(loadingScene);
			if (!loadingSceneMetadata || loadingSceneMetadata->Type != AssetType::Scene)
			{
				return fail(fmt::format("The project's loading scene {} is not a scene asset of the project; set another or none with "
					"project.setExportSettings", loadingScene.ToString()));
			}
		}

		AssetPackBuildOptions packOptions;
		packOptions.StartScene = startScene;
		packOptions.Prune = options.Prune;
		packOptions.AlwaysInclude = project.GetConfig().Export.AlwaysInclude;
		if (loadingSceneMetadata)
			packOptions.AlwaysInclude.push_back(loadingSceneMetadata->Path);
		if (options.Prune)
		{
			// What AlwaysInclude names is checked now, without planning: the plan that is packed is made once, when imports
			// are done.
			std::string includeError;
			if (!assets->CheckAlwaysInclude(packOptions.AlwaysInclude, &includeError))
				return fail(includeError);
		}

		if (!FileSystem::CreateDirectories(options.Directory))
			return fail(fmt::format("Could not create the export directory '{}': check that it lies on a writable drive", FileSystem::ToUTF8(options.Directory)));

		Ref<GameExport> exporter(new GameExport());
		exporter->m_Options = options;
		exporter->m_Assets = std::static_pointer_cast<EditorAssetManager>(assets->shared_from_this());
		exporter->m_PackOptions = std::move(packOptions);
		exporter->m_FileName = ToFileName(project.GetConfig().Name);
		exporter->m_Runtime = runtime;
		exporter->m_ScriptModulePath = scriptModule.Path;
		exporter->m_ScriptModuleBytes = std::move(scriptModule.Bytes);
		exporter->m_Manifest.Name = project.GetConfig().Name;
		exporter->m_Manifest.AssetPack = exporter->m_FileName + ".stpak";
		exporter->m_Manifest.StartScene = startScene;
		exporter->m_Manifest.LoadingScene = loadingScene;
		if (!scriptModule.Path.empty())
			exporter->m_Manifest.ScriptModule = FileSystem::ToUTF8(scriptModule.Path.filename());
		exporter->m_Manifest.WindowWidth = options.WindowWidth;
		exporter->m_Manifest.WindowHeight = options.WindowHeight;
		exporter->m_Manifest.Fullscreen = options.Fullscreen;
		exporter->m_StartTime = std::chrono::steady_clock::now();
		exporter->m_Shared = CreateRef<Shared>();
		ST_INFO("Exporting '{}' to {}", project.GetConfig().Name, FileSystem::ToUTF8(options.Directory));
		return exporter;
	}

	GameExport::~GameExport()
	{
		// The I/O job reads from the asset manager this export keeps: it ends first.
		if (m_Shared)
			m_Shared->Cancel = true;
		if (m_Job.IsValid())
			m_Job.Wait();
	}

	void GameExport::Update()
	{
		if (m_Collected)
			return;
		if (m_Shared->State == GameExportState::WaitingForImports)
		{
			if (m_Shared->Cancel)
			{
				{
					std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
					m_Shared->Ended = true;
					m_Shared->Outcome = GameExportState::Cancelled;
					m_Shared->Error = "The export was cancelled";
				}
				Collect();
				return;
			}
			// Finished imports are applied by the asset manager's update, which runs every frame.
			if (m_Assets->HasPendingImports())
				return;
			Begin();
		}
		Collect();
	}

	void GameExport::Begin()
	{
		AssetPackBuildPlan plan;
		std::string error;
		if (!m_Assets->PlanAssetPack(m_PackOptions, plan, &error))
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			m_Shared->Ended = true;
			m_Shared->Outcome = GameExportState::Failed;
			m_Shared->Error = std::move(error);
			return;
		}

		// Progress in bytes: the stored forms the pack writer reads (as it weighs them), then the files the export copies.
		uint64_t packBytes = 0;
		for (const AssetPackSource& source : plan.Assets)
			packBytes += std::max<uint64_t>(source.Metadata.StoredSize, 1);
		uint64_t copyBytes = m_ScriptModuleBytes.size();
		if (m_Options.IncludeRuntime)
			copyBytes += FileSystem::GetFileSize(m_Runtime).value_or(0);
		m_AssetCount = static_cast<uint32_t>(plan.Assets.size());
		m_Shared->TotalBytes = std::max<uint64_t>(packBytes + copyBytes, 1);
		m_Shared->State = GameExportState::Packing;

		// Everything the job uses is its own (or, for the asset manager, kept alive by this export until the job ended).
		struct JobData
		{
			AssetPackBuildPlan Plan;
			GameExportOptions Options;
			GameManifest Manifest;
			std::string FileName;
			std::filesystem::path Runtime;
			std::filesystem::path ScriptModulePath;
			std::vector<uint8_t> ScriptModuleBytes;
			uint64_t PackBytes = 0;
		};
		Ref<JobData> data = CreateRef<JobData>(JobData { std::move(plan), m_Options, m_Manifest, m_FileName, m_Runtime, m_ScriptModulePath,
			std::move(m_ScriptModuleBytes), packBytes });
		Ref<Shared> shared = m_Shared;
		EditorAssetManager* assets = m_Assets.get();

		auto job = [data, shared, assets]()
		{
			auto end = [&shared](GameExportState outcome, std::string error, GameExportResult result = {})
			{
				std::scoped_lock<std::mutex> lock(shared->Mutex);
				shared->Ended = true;
				shared->Outcome = outcome;
				shared->Error = std::move(error);
				shared->Result = std::move(result);
			};
			auto cancelled = [&shared]() { return shared->Cancel.load(); };

			try
			{
				const GameExportOptions& options = data->Options;
				if (data->ScriptModulePath.empty())
				{
					const std::string user = FindAssetUsingScripts(*assets, data->Plan);
					if (!user.empty())
					{
						return end(GameExportState::Failed, fmt::format("'{}' uses scripts, but no script module is loaded: build the scripts (script.build) "
							"or load the built module (script.reload)", user));
					}
				}
				GameExportResult result;
				result.AssetPack = options.Directory / FileSystem::FromUTF8(data->Manifest.AssetPack);
				AssetPackWriteOptions writeOptions;
				writeOptions.Cancel = &shared->Cancel;
				writeOptions.Progress = [&shared](uint64_t done, uint64_t) { shared->DoneBytes = done; };
				std::string error;
				if (!assets->WriteAssetPack(result.AssetPack, data->Plan, writeOptions, &result.Pack, &error))
				{
					if (cancelled())
						end(GameExportState::Cancelled, "The export was cancelled");
					else
						end(GameExportState::Failed, fmt::format("Building the asset pack failed: {}", error));
					return;
				}
				result.AssetCount = data->Plan.Assets.size();
				result.ExcludedAssets = data->Plan.Excluded;
				shared->DoneBytes = data->PackBytes;
				shared->State = GameExportState::Copying;

				if (!data->ScriptModulePath.empty())
				{
					result.ScriptModule = options.Directory / data->ScriptModulePath.filename();
					if (!FileSystem::WriteBytes(result.ScriptModule, data->ScriptModuleBytes))
						return end(GameExportState::Failed, DescribeWriteFailure("the script module", result.ScriptModule));
					std::filesystem::path symbols = data->ScriptModulePath;
					symbols.replace_extension(".pdb");
					if (options.IncludeScriptSymbols && FileSystem::IsRegularFile(symbols) && !FileSystem::Copy(symbols, options.Directory / symbols.filename(), true))
						return end(GameExportState::Failed, fmt::format("Could not copy the script module's symbols '{}'", FileSystem::ToUTF8(symbols)));
					shared->DoneBytes += data->ScriptModuleBytes.size();
				}
				if (cancelled())
					return end(GameExportState::Cancelled, "The export was cancelled");

				result.Manifest = options.Directory / FileSystem::FromUTF8(data->FileName + std::string(GameManifest::c_FileExtension));
				std::string manifestError;
				if (!data->Manifest.Save(result.Manifest, &manifestError))
					return end(GameExportState::Failed, DescribeWriteFailure("the game manifest", result.Manifest));

				if (options.IncludeRuntime)
				{
					result.Executable = options.Directory / FileSystem::FromUTF8(GetExecutableFileName(data->FileName));
					if (!FileSystem::Copy(data->Runtime, result.Executable, true))
					{
						return end(GameExportState::Failed, fmt::format("{} from '{}' (is the game running from there? close it first)",
							DescribeWriteFailure("the runtime", result.Executable), FileSystem::ToUTF8(data->Runtime)));
					}
					if (!FileSystem::Copy(data->Runtime.parent_path() / c_NoticesFile, options.Directory / c_NoticesFile, true))
						return end(GameExportState::Failed, fmt::format("Could not copy '{}'", c_NoticesFile));
#if !defined(ST_PLATFORM_WINDOWS)
					std::error_code permissionError;
					std::filesystem::permissions(result.Executable, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec
						| std::filesystem::perms::others_exec, std::filesystem::perm_options::add, permissionError);
#endif
				}
				shared->DoneBytes = shared->TotalBytes.load();
				end(GameExportState::Succeeded, {}, std::move(result));
			}
			catch (const std::exception& exception)
			{
				end(GameExportState::Failed, fmt::format("The export failed: {}", exception.what()));
			}
		};

		try
		{
			m_Job = JobSystem::SubmitIO(std::move(job), JobPriority::Normal);
		}
		catch (const std::exception& exception)
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			m_Shared->Ended = true;
			m_Shared->Outcome = GameExportState::Failed;
			m_Shared->Error = fmt::format("Could not start the export: {}", exception.what());
		}
	}

	void GameExport::Collect()
	{
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			if (!m_Shared->Ended)
				return;
			m_Result = std::move(m_Shared->Result);
			m_Error = m_Shared->Error;
			m_Shared->State = m_Shared->Outcome;
		}
		// The job ended: wait for its handle too, so that nothing of it runs any more.
		if (m_Job.IsValid())
			m_Job.Wait();
		m_Job = {};
		m_Collected = true;
		m_Assets.reset();
		m_Result.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_StartTime).count();

		const GameExportState state = m_Shared->State;
		if (state == GameExportState::Succeeded)
		{
			ST_INFO("Exported '{}' to {} ({} assets, {:.1f} MB pack, {:.1f} s)", m_Manifest.Name, FileSystem::ToUTF8(m_Options.Directory), m_Result.AssetCount,
				static_cast<double>(m_Result.Pack.FileSize) / (1024.0 * 1024.0), m_Result.Seconds);
		}
		else if (state == GameExportState::Failed)
		{
			ST_ERROR("Exporting '{}' failed: {}", m_Manifest.Name, m_Error);
		}
		else
		{
			ST_INFO("Exporting '{}' was cancelled", m_Manifest.Name);
		}
	}

	void GameExport::Cancel()
	{
		if (!m_Collected)
			m_Shared->Cancel = true;
	}

	void GameExport::Wait()
	{
		if (m_Collected)
			return;
		if (m_Job.IsValid())
			m_Job.Wait();
		else if (m_Shared->State == GameExportState::WaitingForImports && m_Shared->Cancel)
		{
			std::scoped_lock<std::mutex> lock(m_Shared->Mutex);
			m_Shared->Ended = true;
			m_Shared->Outcome = GameExportState::Cancelled;
			m_Shared->Error = "The export was cancelled";
		}
		Collect();
	}

	GameExportState GameExport::GetState() const
	{
		return m_Shared->State;
	}

	bool GameExport::IsFinished() const
	{
		return m_Collected;
	}

	float GameExport::GetProgress() const
	{
		if (m_Collected)
			return m_Shared->State == GameExportState::Succeeded ? 1.0f : 0.0f;
		const double done = static_cast<double>(m_Shared->DoneBytes.load());
		const double total = static_cast<double>(m_Shared->TotalBytes.load());
		return static_cast<float>(std::clamp(done / total, 0.0, 1.0));
	}

	std::string GameExport::GetStage() const
	{
		auto megabytes = [](uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); };
		switch (m_Shared->State.load())
		{
			case GameExportState::WaitingForImports:
				return "Waiting for asset imports to finish";
			case GameExportState::Packing:
				return fmt::format("Packing {} {} ({:.0f} of {:.0f} MB)", m_AssetCount, m_AssetCount == 1 ? "asset" : "assets",
					megabytes(std::min(m_Shared->DoneBytes.load(), m_Shared->TotalBytes.load())), megabytes(m_Shared->TotalBytes.load()));
			case GameExportState::Copying:
				return "Writing the script module, the manifest and the runtime";
			case GameExportState::Succeeded:
				return fmt::format("Exported {} {} to {}", m_Result.AssetCount, m_Result.AssetCount == 1 ? "asset" : "assets", FileSystem::ToUTF8(m_Options.Directory));
			case GameExportState::Failed:
				return m_Error;
			case GameExportState::Cancelled:
				return "Cancelled";
		}
		return {};
	}

}
