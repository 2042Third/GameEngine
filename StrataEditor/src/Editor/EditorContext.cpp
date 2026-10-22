#include "Editor/EditorContext.h"

#include "Editor/CommandJournal.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorPreferences.h"
#include "Editor/EditorWorld.h"
#include "Editor/ExternalEditor.h"
#include "Editor/GameExport.h"
#include "Editor/InputSequence.h"
#include "Editor/ProjectSamples.h"
#include "Editor/ProjectThumbnail.h"
#include "Editor/SceneRecovery.h"
#include "Editor/SettingsFileWriter.h"
#include "Editor/ScriptProject.h"
#include "Editor/ThumbnailCache.h"
#include "Editor/WorldCook.h"

#include <Strata/Asset/AssetDependencies.h>
#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/FileReader.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Profiling.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Input/Input.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/PrefabOverrides.h>
#include <Strata/Scene/ProjectCollisionLayers.h>
#include <Strata/Scene/SceneLoader.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/Scene/SceneWorkBudget.h>
#include <Strata/Scripting/ScriptingComponents.h>
#include <Strata/World/WorldStreamingSystem.h>

#include <algorithm>
#include <unordered_set>
#include <limits>
#include <string_view>
#include <utility>

namespace Strata
{

	const char* SceneStateToString(SceneState state)
	{
		switch (state)
		{
			case SceneState::Edit:     return "Edit";
			case SceneState::Play:     return "Play";
			case SceneState::Simulate: return "Simulate";
		}
		return "Unknown";
	}

	namespace
	{

		constexpr const char* c_ViewportStateFile = "EditorViewport.json";
		// Deleted assets (asset.delete) wait in "<project>/.strata/Trash" this long, then go when the project is opened.
		constexpr const char* c_TrashDirectory = "Trash";
		constexpr const char* c_ThumbnailDirectory = "Thumbnails";
		constexpr std::chrono::hours c_TrashKeepTime { 30 * 24 };
		// A scene's first view is framed again when its meshes load within this time (EditorContext::OpenScene).
		constexpr std::chrono::seconds c_PendingFrameTimeout { 30 };

		// The asset manager of an editor without a project: only the built-in assets (primitive meshes, default material),
		// which are memory assets, so nothing is ever read from storage.
		class BuiltinAssetManager final : public AssetManagerBase
		{
		public:
			~BuiltinAssetManager() override
			{
				WaitForInFlightLoads();
			}
		protected:
			bool ReadAssetData(const AssetMetadata& metadata, std::vector<uint8_t>&, std::string* outError) override
			{
				if (outError)
					*outError = fmt::format("'{}' is not available without a project", metadata.Name);
				return false;
			}
		};

		std::optional<Sha256Digest> HashFile(const std::filesystem::path& path)
		{
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
			return bytes ? std::optional<Sha256Digest>(Sha256::Hash(*bytes)) : std::nullopt;
		}

		// Whether a scene or prefab document attaches a script: its JSON names the Script component (the key "Script" followed
		// by a colon; a name or text "Script" is a value, and quotes inside strings are escaped). Read in chunks, so the
		// largest scenes cost no memory; stops early when cancelled. Any thread.
		bool DocumentAttachesScripts(const std::filesystem::path& path, const std::atomic<bool>& cancelled)
		{
			constexpr std::string_view c_Key = "\"Script\"";
			constexpr size_t c_ChunkSize = 1 << 20;
			const Scope<FileReader> reader = FileReader::Open(path);
			if (!reader)
				return false;
			std::vector<uint8_t> chunk;
			std::string window; // The end of the previous chunk, so that a key across chunks is found
			for (uint64_t offset = 0; offset < reader->GetSize() && !cancelled; offset += chunk.size())
			{
				chunk.resize(static_cast<size_t>(std::min<uint64_t>(c_ChunkSize, reader->GetSize() - offset)));
				if (!reader->ReadAt(offset, chunk))
					return false;
				window.append(chunk.begin(), chunk.end());
				for (size_t found = window.find(c_Key); found != std::string::npos; found = window.find(c_Key, found + 1))
				{
					size_t next = found + c_Key.size();
					while (next < window.size() && (window[next] == ' ' || window[next] == '\t' || window[next] == '\r' || window[next] == '\n'))
						next++;
					if (next < window.size() && window[next] == ':')
						return true;
				}
				// Keep enough of the end for a key and the blanks before its colon to straddle the next chunk.
				constexpr size_t c_Keep = 64;
				if (window.size() > c_Keep)
					window.erase(0, window.size() - c_Keep);
			}
			return false;
		}

	}

	EditorContext::EditorContext(const EditorContextSpecification& specification)
		: m_Specification(specification), m_EditScene(CreateRef<Scene>()), m_Journal(CreateRef<CommandJournal>()),
		  m_Recovery(CreateScope<SceneRecovery>()), m_ProjectThumbnail(CreateScope<ProjectThumbnail>()),
		  m_Preferences(CreateScope<EditorPreferences>(GetSettingsRegistry(), specification.PreferencesFile, specification.EditorSettingsFile,
			  [this](const std::string& error) { OnPreferencesWritten(error); })),
		  m_ExternalEditor(CreateScope<ExternalEditorLauncher>(*m_Preferences)),
		  m_RecentProjects(specification.RecentProjectsFile, specification.RecentProjectsReadOnly)
	{
		ActivateBuiltinAssets();
		if (specification.ToolchainEnvironment)
			m_ScriptBuilder.SetToolchainEnvironment(specification.ToolchainEnvironment);
		m_GameWindowProvider = CreateScope<EngineServices::ScopedProvider<IGameWindow>>(m_GameWindow);
		m_GameSavesProvider = CreateScope<EngineServices::ScopedProvider<IGameSaves>>(m_GameSaves);
		m_ProjectSettingsProvider = CreateScope<EngineServices::ScopedProvider<IProjectSettings>>(m_OpenProjectSettings);
		// What the game asked of its shell: window requests (recorded, not applied), its cursor, where its saves go.
		SetStatusProvider("game", [this]()
		{
			const std::optional<std::filesystem::path> saves = GetGameSaveDirectory();
			const CursorMode cursor = Input::GetGameCursorMode();
			return nlohmann::json { { "windowRequests", m_GameWindow.Describe() },
				{ "cursor", cursor == CursorMode::Locked ? "locked" : (cursor == CursorMode::Hidden ? "hidden" : "normal") },
				{ "saveDirectory", saves ? nlohmann::json(FileSystem::ToUTF8(*saves)) : nlohmann::json(nullptr) } };
		});
		// The recovery journal follows every step the undo stack applies; recovery files apply only to scenes the project
		// still has, which scans find by their handles wherever they moved.
		m_UndoStack.AddObserver(m_Recovery.get());
		m_Recovery->BeginScene(*m_EditScene, UUID::Null(), {}, {});
		CreateAnimationPreview();
		m_Recovery->SetEntryValidator([this](const RecoveryEntry& entry) -> std::string
		{
			if (!entry.Scene.IsValid())
				return {};
			if (!m_AssetManager || m_AssetManager->GetAssetType(entry.Scene) != AssetType::Scene)
				return fmt::format("The scene {} ('{}') is no longer in the project", entry.Scene.ToString(), entry.ScenePath);
			return {};
		});
		m_Recovery->SetSceneLocator([this]()
		{
			SceneRecovery::ScenePaths scenes;
			if (m_AssetManager)
			{
				for (const AssetMetadata& metadata : m_AssetManager->GetAllMetadata(AssetType::Scene))
					scenes.emplace(metadata.Handle, metadata.Path);
			}
			return scenes;
		});
		SetStatusProvider("recovery", [this]() { return m_Recovery->DescribeStatus(); });
		// The world the edited scene is, when it is one (world.status has the details).
		SetStatusProvider("world", [this]() { return CommandUtils::DescribeWorldSummary(*this); });
		// The asset manager of the open project, or the built-in assets' without one (asset.stats has the details). Imports
		// still running (a project opened with assets to import, re-imports) deliver their assets when they finish; scanning
		// is the scan of the project's files while it registers them (null: none runs).
		SetStatusProvider("assets", [this]()
		{
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			if (!manager)
				return nlohmann::json(nullptr);
			nlohmann::json description = CommandUtils::DescribeAssetStats(manager->GetStats());
			description["importing"] = m_AssetManager && manager == m_AssetManager ? m_AssetManager->GetPendingImportCount() : 0u;
			// The background scan of the project's assets, while it runs (commands that need the whole registry wait for it).
			description["scanning"] = CommandUtils::DescribeAssetScan(*this);
			// Documents changed on disk whose search of what they reference waits or runs (no import: exports do not wait for it).
			description["referenceSearches"] = m_AssetManager && manager == m_AssetManager ? m_AssetManager->GetPendingReferenceSearches().size() : size_t(0);
			// The imports' memory estimates against their budget (EditorAssetManager::SetImportMemoryBudget).
			description["importMemoryMB"] = m_AssetManager && manager == m_AssetManager ? CommandUtils::DescribeImportMemory(m_AssetManager->GetImportMemoryStats())
				: nlohmann::json(nullptr);
			return description;
		}, [this]()
		{
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			if (!manager)
				return nlohmann::json(nullptr);
			const AssetManagerStats stats = manager->GetStats();
			return nlohmann::json {
				{ "ready", stats.LoadedAssets },
				{ "loading", stats.LoadingAssets },
				{ "importing", m_AssetManager && manager == m_AssetManager ? m_AssetManager->GetPendingImportCount() : 0u },
				{ "scanning", CommandUtils::DescribeAssetScan(*this) },
				{ "failed", stats.FailedAssets } };
		});
	}

	EditorContext::~EditorContext()
	{
		ReleaseProject(false);
		// A preferences write still in flight ends here, while everything its completion uses (the notifications) is there.
		m_Preferences->Finish();
		if (m_BuiltinAssets && AssetManager::GetActive() == m_BuiltinAssets)
			AssetManager::SetActive(nullptr);
	}

	void EditorContext::ActivateBuiltinAssets()
	{
		if (!m_BuiltinAssets)
			m_BuiltinAssets = CreateRef<BuiltinAssetManager>();
		AssetManager::SetActive(m_BuiltinAssets);
	}

	std::filesystem::path EditorContext::GetViewportStateFile() const
	{
		return m_Project ? m_Project->GetIntermediateDirectory() / c_ViewportStateFile : std::filesystem::path();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Project
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::CreateProject(const std::filesystem::path& directory, const std::string& name, std::string_view templateId, std::string* outError)
	{
		if (!ProjectTemplates::Find(templateId))
		{
			if (outError)
				*outError = fmt::format("Unknown project template '{}' (templates: {})", templateId, ProjectTemplates::ListIds());
			return false;
		}
		Ref<Project> project = Project::Create(directory, name, outError);
		if (!project)
			return false;
		// New projects come with their script build and an example script.
		if (!CreateScriptProjectFiles(*project, true, nullptr, outError, &GetScriptBuildSettings()))
			return false;
		if (!OpenProjectInternal(project->GetProjectFile(), true, outError))
			return false;
		return !ProjectTemplates::HasStartScene(templateId) || SaveTemplateStartScene(templateId, outError);
	}

	bool EditorContext::SaveTemplateStartScene(std::string_view templateId, std::string* outError)
	{
		if (!NewScene("Main", templateId))
		{
			if (outError)
				*outError = fmt::format("Unknown project template '{}'", templateId);
			return false;
		}
		std::string error;
		if (!SaveSceneAs(std::string(ProjectTemplates::c_StartScenePath), &error))
		{
			if (outError)
				*outError = fmt::format("The project was created, but its start scene could not be saved: {}", error);
			return false;
		}
		// Written in the background like every change of the project's settings; a failure is reported as theirs are.
		m_Project->GetConfig().StartScene = m_SceneHandle;
		SaveProjectFile();
		return true;
	}

	void EditorContext::SaveProjectFile()
	{
		if (!m_Project)
			return;
		// The snapshot: a copy of the project's small in-memory values, serialized and written on the writer's job.
		Ref<const Project> snapshot = CreateRef<Project>(*m_Project);
		m_ProjectFileWriter->Submit([snapshot](std::string* outError) { return snapshot->Save(outError); });
	}

	SettingsFileState EditorContext::GetProjectFileState() const
	{
		return m_ProjectFileWriter ? m_ProjectFileWriter->GetState() : SettingsFileState::Written;
	}

	std::string EditorContext::GetProjectFileError() const
	{
		return GetProjectFileState() == SettingsFileState::Unwritten ? m_ProjectFileWriter->GetLastError() : std::string();
	}

	bool EditorContext::HasUnwrittenProjectSettings() const
	{
		return GetProjectFileState() == SettingsFileState::Unwritten;
	}

	const SettingsRegistry& EditorContext::GetSettingsRegistry() const
	{
		return m_Specification.Settings ? *m_Specification.Settings : SettingsRegistry::GetProcessRegistry();
	}

	bool EditorContext::HasSavesInFlight() const
	{
		return m_DocumentSaver.IsBusy() || (m_ProjectFileWriter && m_ProjectFileWriter->IsBusy()) || m_Preferences->IsWriting();
	}

	bool EditorContext::HasUnwrittenPreferences() const
	{
		return m_Preferences->HasUnwrittenPreferences();
	}

	void EditorContext::OnPreferencesWritten(const std::string& error)
	{
		if (error.empty())
			return;
		const std::string file = FileSystem::ToUTF8(m_Preferences->GetFile().filename());
		ST_ERROR("Writing the preferences file '{}' failed: {} (the change is kept in memory; the next change or preferences.save writes it "
			"again)", FileSystem::ToUTF8(m_Preferences->GetFile()), error);
		NotifySaveFailed(file, fmt::format("{}. The preferences are kept in memory: the next change, or preferences.save, writes them again.", error),
			UUID::Null());
	}

	void EditorContext::OnProjectFileWritten(const std::string& error)
	{
		if (error.empty())
			return;
		const std::string file = FileSystem::ToUTF8(m_Project->GetProjectFile().filename());
		ST_ERROR("Writing the project file '{}' failed: {} (the change is kept in memory; the next change or project.save writes it again)", file,
			error);
		NotifySaveFailed(file, fmt::format("{}. The project's settings are kept in memory: the next change, or project.save, writes them again.",
			error), UUID::Null());
	}

	bool EditorContext::BeginOpenProject(const std::filesystem::path& path, std::string* outError)
	{
		return OpenProjectInternal(path, false, outError);
	}

	bool EditorContext::OpenProject(const std::filesystem::path& path, std::string* outError)
	{
		if (!BeginOpenProject(path, outError))
			return false;
		// The project is open whatever becomes of its start scene (a failure is logged as a warning).
		WaitForSceneLoad();
		return true;
	}

	bool EditorContext::OpenProjectInternal(const std::filesystem::path& path, bool created, std::string* outError)
	{
		std::filesystem::path projectFile = path;
		if (FileSystem::IsDirectory(path))
		{
			projectFile = Project::FindProjectFile(path, outError);
			if (projectFile.empty())
				return false;
		}
		Ref<Project> project = Project::Load(projectFile, outError, &GetSettingsRegistry());
		if (!project)
			return false;

		CloseProject();
		m_Project = project;
		m_ProjectGeneration++;
		Project::SetActive(m_Project);
		m_ProjectFileWriter = CreateScope<SettingsFileWriter>([this](const std::string& error) { OnProjectFileWritten(error); });

		// The editor camera and viewport settings continue where they were when the project was last closed.
		const std::filesystem::path viewportState = GetViewportStateFile();
		std::string viewportError;
		if (FileSystem::Exists(viewportState) && !m_Viewport.Load(viewportState, &viewportError))
			ST_WARN("The saved viewport state is ignored: {}", viewportError);

		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = m_Project->GetAssetDirectory();
		specification.CacheDirectory = m_Project->GetCacheDirectory();
		specification.TrashDirectory = m_Project->GetIntermediateDirectory() / c_TrashDirectory;
		specification.WatchFiles = m_Specification.WatchAssetFiles;
		m_AssetManager = CreateRef<EditorAssetManager>(specification);
		AssetManager::SetActive(m_AssetManager);
		m_DocumentSaver.SetAssetManager(m_AssetManager);
		// In the background, the project opens at once: the scan's jobs prepare the assets while frames go on, and what needs
		// the whole registry waits for it (commands, the start scene, the script usage scan).
		if (m_Specification.ImportInBackground)
			m_AssetManager->BeginScan(ScanImports::InBackground);
		else
			m_AssetManager->Scan(ScanImports::Wait);
		m_AssetManager->PurgeTrash(c_TrashKeepTime);
		OpenThumbnails();
		ST_INFO("Opened project '{}' ({})", m_Project->GetConfig().Name, FileSystem::ToUTF8(m_Project->GetProjectDirectory()));
		// Unsaved changes an editor left behind (a crash) or discarded are offered once the scan has read them. They are judged
		// against the asset registry (does their scene still exist, where is it now): a scan of the assets in the background
		// starts the recovery scan once it registered them (FinishAssetScan); until then the recovery counts as scanning, so
		// that the offer as the project opens waits for its entries.
		m_Recovery->SetProject(m_Project->GetIntermediateDirectory() / SceneRecovery::c_DirectoryName, m_Project->GetAssetDirectory());
		if (m_AssetManager->IsScanning())
			m_Recovery->ExpectScan();
		else
			m_Recovery->Scan();
		OpenScriptEngine(created);
		// A project saved by 0.5.0 or older kept its collision layers in its scenes: they are read on a job before the start
		// scene opens, so that it opens with the project's.
		if (m_Project->GetFileVersion() < Project::c_FormatVersion)
			BeginProjectMigration();

		// Continue with the start scene when the project has one: it loads in the background (UpdateSceneLoad warns when it
		// cannot be opened), once the assets are registered and the migration is done. Meanwhile it counts as the scene being
		// opened (IsSceneLoading).
		const AssetHandle startScene = m_Project->GetConfig().StartScene;
		const bool scanning = m_AssetManager->IsScanning();
		if (scanning)
			m_AfterAssetScan = true;
		else
			StartScriptUsageScan();
		if (startScene.IsValid() && (scanning || m_ProjectMigration))
		{
			CancelSceneLoad();
			m_StartSceneAfterScan = startScene;
			m_SceneLoadStatus = EditorSceneLoadStatus();
			m_SceneLoadStatus.ID = m_NextSceneLoadID++;
			m_SceneLoadStatus.Scene = startScene;
			m_SceneLoadStatus.State = SceneLoadState::LoadingScene;
			m_SceneLoadStatus.StartScene = true;
			m_SceneLoadStatus.Started = std::chrono::steady_clock::now();
		}
		else if (startScene.IsValid())
		{
			OpenStartScene(startScene, m_NextSceneLoadID);
		}
		m_RecentProjects.Add(m_Project->GetConfig().Name, m_Project->GetProjectFile());
		return true;
	}

	void EditorContext::OpenStartScene(AssetHandle startScene, uint64_t id)
	{
		std::string sceneError;
		if (!BeginOpenScene(startScene, &sceneError))
		{
			ST_WARN("Could not open the start scene: {}", sceneError);
			// Reported like a load that failed (project.open, scene.loadStatus).
			m_SceneLoadStatus = EditorSceneLoadStatus();
			m_SceneLoadStatus.Scene = startScene;
			m_SceneLoadStatus.State = SceneLoadState::Failed;
			m_SceneLoadStatus.Error = std::move(sceneError);
			m_SceneLoadStatus.Started = std::chrono::steady_clock::now();
		}
		// The load the project's opening announced (its ID) goes on as this one.
		m_SceneLoadStatus.ID = id;
		m_NextSceneLoadID = std::max(m_NextSceneLoadID, id + 1);
		m_SceneLoadStatus.StartScene = true;
	}

	void EditorContext::FinishAssetScan()
	{
		if (m_AfterAssetScan && m_AssetManager && !m_AssetManager->IsScanning())
		{
			m_AfterAssetScan = false;
			m_Recovery->Scan();
			StartScriptUsageScan();
		}
		// The start scene waits for the whole registry and for the migration of a project saved by 0.5.0 or older.
		if (m_AfterAssetScan || m_ProjectMigration || !m_StartSceneAfterScan.IsValid())
			return;
		const AssetHandle startScene = std::exchange(m_StartSceneAfterScan, UUID::Null());
		OpenStartScene(startScene, m_SceneLoadStatus.ID);
		m_StartSceneAfterScanID = m_SceneLoadStatus.ID;
	}

	void EditorContext::BeginProjectMigration()
	{
		// Without the section (a test's own registry) the layers have nowhere to go: the project stays as it is.
		if (!GetSettingsRegistry().Find(ProjectCollisionLayers::c_SectionId))
			return;
		m_ProjectMigration = CreateScope<PendingMigration>();
		m_ProjectMigration->Cancel = std::make_shared<std::atomic<bool>>(false);
		m_ProjectMigration->Scan = std::make_shared<ProjectMigration::SceneLayerScan>();
		ST_INFO("The project '{}' was saved by 0.5.0 or older (project file version {}): reading its scenes' collision layers to make them the "
			"project's", m_Project->GetConfig().Name, m_Project->GetFileVersion());
		m_ProjectMigration->Job = JobSystem::SubmitIO([directory = m_Project->GetAssetDirectory(), cancel = m_ProjectMigration->Cancel,
			scan = m_ProjectMigration->Scan]()
		{
			*scan = ProjectMigration::ScanSceneLayers(directory, cancel.get());
		});
	}

	void EditorContext::UpdateProjectMigration(bool wait)
	{
		if (!m_ProjectMigration)
			return;
		if (wait)
			JobSystem::Wait(m_ProjectMigration->Job);
		else if (!m_ProjectMigration->Job.IsComplete())
			return;
		const Scope<PendingMigration> migration = std::move(m_ProjectMigration);
		const ProjectMigration::SceneLayerScan& scan = *migration->Scan;
		const ProjectMigration::LayerMerge merge = ProjectMigration::MergeSceneLayers(scan.Scenes, m_Project->GetConfig().StartScene);
		std::vector<std::string> unreadable = scan.Problems;
		// No undo step: the project had no table to go back to.
		if (merge.Layers && !merge.Layers->IsDefault())
		{
			std::string error;
			if (!m_Project->GetSettings().Set(ProjectCollisionLayers::c_SectionId, merge.Layers->ToJson(), GetSettingsRegistry(), &error))
				unreadable.push_back(fmt::format("The scenes' collision layers could not become the project's: {}", error));
		}
		// The version records that the migration ran, so the file is written as version 3 also when no table resulted: a
		// project migrates once, and a table reset later never comes back from the scenes.
		SaveProjectFile();
		auto list = [](const std::vector<std::string>& items, std::string_view separator)
		{
			std::string text;
			for (const std::string& item : items)
				text += (text.empty() ? "" : std::string(separator)) + item;
			return text;
		};
		if (!merge.Scenes.empty())
		{
			ST_INFO("The collision layers of {} scene(s) are the project's now (Project Settings > Collision Layers): {}", merge.Scenes.size(),
				list(merge.Scenes, ", "));
		}
		if (!merge.Conflicts.empty() || !unreadable.empty())
		{
			std::vector<std::string> problems = merge.Conflicts;
			problems.insert(problems.end(), unreadable.begin(), unreadable.end());
			ST_WARN("Making the scenes' collision layers the project's: {}", list(problems, "; "));
		}
		m_ProjectMigrationResult = { { "collisionLayers", {
			{ "scenes", merge.Scenes },
			{ "conflicts", merge.Conflicts },
			{ "unreadable", unreadable } } } };
		ApplyProjectLayers();
	}

	void EditorContext::ApplyProjectLayers()
	{
		m_EditScene->ApplyProjectLayers();
		if (m_RuntimeScene)
			m_RuntimeScene->ApplyProjectLayers();
	}

	void EditorContext::AdoptProjectLayers(Scene& scene, AssetHandle handle)
	{
		scene.ApplyProjectLayers();
		if (!scene.DoStoredLayersDiffer() || !handle.IsValid() || !m_StoredLayersWarned.insert(handle).second)
			return;
		const std::string path = GetSceneFile(handle).second;
		ST_WARN("The scene '{}'{} stores collision layers of its own, as 0.5.0 kept them: the project's apply (Project Settings > Collision "
			"Layers), and saving the scene drops the scene's", scene.GetName(), path.empty() ? std::string() : fmt::format(" ({})", path));
	}

	bool EditorContext::IsScanningAssets() const
	{
		return m_AssetManager && m_AssetManager->IsScanning();
	}

	bool EditorContext::IsWaitingForAssets() const
	{
		return IsScanningAssets() || m_StartSceneAfterScan.IsValid() || (m_SceneLoad && m_SceneLoadStatus.ID == m_StartSceneAfterScanID);
	}

	std::optional<AssetScanProgress> EditorContext::GetAssetScanProgress() const
	{
		return m_AssetManager ? m_AssetManager->GetScanProgress() : std::nullopt;
	}

	void EditorContext::OpenThumbnails()
	{
		m_Thumbnails.reset();
		// Without a GPU nothing could show them.
		if (!Renderer::IsInitialized() || !m_AssetManager || !m_Project)
			return;
		ThumbnailCacheSpecification specification;
		specification.Directory = m_Project->GetIntermediateDirectory() / c_ThumbnailDirectory;
		m_Thumbnails = CreateScope<ThumbnailCache>(m_AssetManager, specification);
	}

	void EditorContext::CloseProject()
	{
		ReleaseProject(true);
	}

	void EditorContext::ReleaseProject(bool activateBuiltinAssets)
	{
		// Saves in flight are written and published before anything of the project goes: closing never loses one. Through
		// commands and the UI the project closes only once they ended, so this blocks only when the editor goes away.
		m_DocumentSaver.FinishAll();
		// So is a write of the project file: the only path that may write it on the main thread (a write that has not started
		// runs here), reached by commands only once nothing was in flight, otherwise only when the editor goes away.
		if (m_ProjectFileWriter)
			m_ProjectFileWriter->Finish();
		// The export reads the project's assets on an I/O thread: it stops before they go.
		if (m_GameExport && !m_GameExport->IsFinished())
		{
			m_GameExport->Cancel();
			m_GameExport->Wait();
		}
		CancelSceneLoad();
		m_AfterAssetScan = false;
		// A migration's scan stops reading the project's scenes; nothing of it is applied.
		if (m_ProjectMigration)
		{
			m_ProjectMigration->Cancel->store(true);
			m_ProjectMigration.reset();
		}
		m_ProjectMigrationResult = nullptr;
		Stop();
		// The world reads the project's assets: it goes before them.
		m_World.reset();
		const bool hadProject = m_Project != nullptr;
		if (hadProject)
		{
			m_Viewport.StoreSceneCamera(m_SceneHandle);
			// Cameras of scenes deleted since are not kept.
			if (m_AssetManager)
				m_Viewport.PruneSceneCameras([this](AssetHandle scene) { return m_AssetManager->GetAssetType(scene) == AssetType::Scene; });
			std::string error;
			if (!m_Viewport.Save(GetViewportStateFile(), &error))
				ST_WARN("The viewport state was not saved: {}", error);
		}
		// Unsaved changes of the scene that goes reach the project's recovery directory before it closes. Without the built-in
		// assets to fall back to, the editor itself goes away, and nobody is left to tell.
		ResetScene(CreateRef<Scene>(), UUID::Null(), activateBuiltinAssets ? DiscardNotice::ProjectCloses : DiscardNotice::None);
		if (hadProject)
			m_Recovery->SetProject({}, {});
		InspectEntities(); // The project's assets go with it
		// After the scene went: the next project starts from the default view, with none of this project's scene cameras.
		if (hadProject)
			m_Viewport.ResetState();
		CloseScriptEngine();
		// Before the asset manager: its reads use the manager.
		m_Thumbnails.reset();
		m_ProjectThumbnail->Cancel();
		if (m_AssetManager)
		{
			// Cancels the project's imports without waiting for them (also while something else still holds the manager).
			m_AssetManager->Close();
			if (AssetManager::GetActive() == m_AssetManager)
				AssetManager::SetActive(nullptr);
			m_DocumentSaver.SetAssetManager(nullptr);
			m_AssetManager.reset();
			m_SceneMetadataVersion = UINT64_MAX;
			m_ImportFailureCount = 0;
		}
		if (m_Project)
		{
			if (Project::GetActive() == m_Project)
				Project::SetActive(nullptr);
			m_ProjectFileWriter.reset();
			m_Project.reset();
			m_ProjectGeneration++;
			// The scene that stays takes the layers of no project.
			ApplyProjectLayers();
		}
		if (activateBuiltinAssets)
			ActivateBuiltinAssets();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Scene
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::ResetScene(Ref<Scene> scene, AssetHandle handle, DiscardNotice notice)
	{
		Stop();
		// Discarded changes name where the scene is now, also when it moved in this frame.
		FollowSceneMoves();
		// Unsaved changes of the scene that goes are written as recovery data first, so nothing is lost whatever replaced
		// it (a forced command, the unsaved-changes dialog's Don't Save, closing the editor).
		std::string lostReason;
		const DiscardOutcome discarded = m_Recovery->EndScene(*m_EditScene, IsSceneModified(), &lostReason);
		if (discarded != DiscardOutcome::Unmodified && notice != DiscardNotice::None)
			NotifyDiscardedChanges(m_EditScene->GetName(), discarded, lostReason, notice);
		// The scene that goes keeps its view for when it opens again.
		m_Viewport.StoreSceneCamera(m_SceneHandle);
		m_PendingFrame.reset();
		m_AnimationPreview.reset(); // Of the scene that goes, which it refers to: before the scene is released
		m_World.reset();             // Likewise
		Ref<Scene> previous = std::exchange(m_EditScene, std::move(scene));
		AdoptProjectLayers(*m_EditScene, handle);
		CreateAnimationPreview();
		m_SceneHandle = handle;
		ClearSelection();
		m_UndoStack.Clear();
		// A world's persistent scene streams its cells around the editor camera.
		if (m_AssetManager)
			m_World = EditorWorld::Attach(*m_AssetManager, *m_EditScene, m_UndoStack);
		// The scene that goes is destroyed off the frame (a million entities take tens of milliseconds).
		Scene::ReleaseInBackground(std::move(previous));
		const auto [sceneFile, scenePath] = GetSceneFile(handle);
		m_Recovery->BeginScene(*m_EditScene, handle, sceneFile, scenePath);
	}

	std::pair<std::filesystem::path, std::string> EditorContext::GetSceneFile(AssetHandle scene) const
	{
		if (!m_AssetManager || !scene.IsValid())
			return {};
		const std::optional<AssetMetadata> metadata = m_AssetManager->GetMetadata(scene);
		if (!metadata || metadata->IsBuiltin() || metadata->IsSubAsset())
			return {};
		return { m_AssetManager->GetAssetDirectory() / FileSystem::FromUTF8(metadata->Path), metadata->Path };
	}

	bool EditorContext::NewScene(const std::string& name, std::string_view templateId)
	{
		if (!ProjectTemplates::Find(templateId))
			return false;
		CancelSceneLoad();
		Ref<Scene> scene = CreateRef<Scene>(name);
		if (!ProjectTemplates::Populate(templateId, *scene, GetTemplateAssets(templateId)))
			return false;
		ResetScene(std::move(scene), UUID::Null());
		if (const std::optional<TemplateView> view = ProjectTemplates::GetEditorView(templateId))
			m_Viewport.GetCamera().LookAt(view->Position, view->Target);
		return true;
	}

	TemplateAssets EditorContext::GetTemplateAssets(std::string_view templateId)
	{
		TemplateAssets assets;
		if (!ProjectTemplates::UsesGroundMaterial(templateId) || !m_AssetManager)
			return assets;
		// The project's ground material, whatever it was changed to; made when it has none.
		const std::string path(ProjectTemplates::c_GroundMaterialPath);
		if (const AssetHandle existing = m_AssetManager->FindAssetByAbsolutePath(m_AssetManager->GetAssetDirectory() / FileSystem::FromUTF8(path));
			existing.IsValid())
		{
			if (m_AssetManager->GetAssetType(existing) == AssetType::Material)
				assets.GroundMaterial = existing;
			else
				ST_WARN("The ground of the new scene has the default material: '{}' is not a material", path);
			return assets;
		}
		const std::string document = ProjectTemplates::GetGroundMaterialDocument();
		std::string error;
		const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.data()), document.size());
		const AssetHandle created = m_AssetManager->CreateNativeAsset(path, bytes, &error);
		if (created.IsValid())
			assets.GroundMaterial = created;
		else
			ST_WARN("The ground of the new scene has the default material: '{}' could not be created: {}", path, error);
		return assets;
	}

	bool EditorContext::BeginOpenScene(AssetHandle handle, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (!m_AssetManager)
			return fail("No project is open");
		if (m_AssetManager->GetAssetType(handle) != AssetType::Scene)
			return fail(fmt::format("{} is not a scene asset of the project", handle.ToString()));

		SceneLoadOptions options;
		// The current file, not a copy loaded earlier.
		options.Reload = true;
		options.Prepare = GetScenePreparation();
		Scope<SceneLoad> next = CreateScope<SceneLoad>();
		next->Request = SceneLoader::LoadAsync(handle, options);
		next->RequestedBy = m_Journal->GetOutermostRunningId();
		// A load under way is replaced: its pins and the loads only it needed go. The new one started first, so that what
		// both use keeps loading (Cancel leaves assets others pin alone).
		CancelSceneLoad();
		m_SceneLoad = std::move(next);
		m_SceneLoadStatus = EditorSceneLoadStatus();
		m_SceneLoadStatus.ID = m_NextSceneLoadID++;
		m_SceneLoadStatus.Scene = handle;
		m_SceneLoadStatus.State = m_SceneLoad->Request.GetState();
		m_SceneLoadStatus.Started = std::chrono::steady_clock::now();
		// It fails at once only for what LoadAsync rejects (checked above already); otherwise Update advances it.
		if (m_SceneLoad->Request.IsFinished())
		{
			UpdateSceneLoad();
			return fail(m_SceneLoadStatus.Error);
		}
		return true;
	}

	std::function<bool(Scene&, SceneWorkBudget&)> EditorContext::GetScenePreparation()
	{
		// The viewport draws a scene from its render proxies: built while the scene waits to replace the shown one, so the
		// frame that swaps it in builds none (a hundred thousand mesh renderers take tens of milliseconds). Without a GPU
		// nothing draws them.
		if (!Renderer::IsInitialized())
			return {};
		return &RenderScene::PrepareLoadedScene;
	}

	bool EditorContext::OpenScene(AssetHandle handle, std::string* outError)
	{
		if (!BeginOpenScene(handle, outError))
			return false;
		if (WaitForSceneLoad())
			return true;
		if (outError)
			*outError = m_SceneLoadStatus.Error;
		return false;
	}

	bool EditorContext::WaitForSceneLoad(std::chrono::milliseconds timeout)
	{
		// A start scene waiting for the asset scan or the project's migration: both finish here (the scan inserts everything
		// at once).
		if (m_AfterAssetScan && m_AssetManager)
			m_AssetManager->Scan(ScanImports::InBackground);
		UpdateProjectMigration(true);
		FinishAssetScan();
		if (!m_SceneLoad)
			return m_SceneLoadStatus.State == SceneLoadState::Ready;
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (m_SceneLoad)
		{
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
			// Everything the request pinned is pending until it is in; waiting for it finalizes it here.
			if (left.count() <= 0 || !m_AssetManager->WaitForPendingLoads(left))
			{
				const bool startScene = m_SceneLoadStatus.StartScene;
				CancelSceneLoad();
				m_SceneLoadStatus.Error = "Loading the scene took too long";
				if (startScene)
					ST_WARN("Could not open the start scene: {}", m_SceneLoadStatus.Error);
				return false;
			}
			UpdateSceneLoad();
			// Loads of assets whose first import still runs in the background (a project opened with ImportInBackground) wait
			// for it and are not pending loads; only applying the import resumes them, which frames do (the manager's Update).
			// Without frames the wait applies imports itself, sleeping until one finishes instead of spinning. A scene being
			// prepared (its render proxies) waits for nothing: other imports must not hold it up.
			if (m_SceneLoad && m_SceneLoadStatus.State != SceneLoadState::Preparing && !m_AssetManager->HasPendingLoads()
				&& m_AssetManager->HasPendingImports())
			{
				const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
				m_AssetManager->WaitForImportProgress(std::max(remaining, std::chrono::milliseconds(0)));
			}
		}
		return m_SceneLoadStatus.State == SceneLoadState::Ready;
	}

	bool EditorContext::CheckSceneEditable(std::string* outError) const
	{
		if (!IsSceneLoading() || IsPlaying())
			return true;
		if (outError)
		{
			*outError = fmt::format("Scene {} is being opened ({:.0f}%): the edited scene is read-only until it replaces it (scene.loadStatus)",
				m_SceneLoadStatus.Scene.ToString(), m_SceneLoadStatus.Progress * 100.0f);
		}
		return false;
	}

	void EditorContext::CancelSceneLoad()
	{
		if (m_StartSceneAfterScan.IsValid())
		{
			m_StartSceneAfterScan = UUID::Null();
			m_SceneLoadStatus.State = SceneLoadState::Cancelled;
			m_SceneLoadStatus.Error = "Opening the scene was cancelled";
			m_SceneLoadStatus.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_SceneLoadStatus.Started).count();
		}
		if (!m_SceneLoad)
			return;
		m_SceneLoad->Request.Cancel();
		m_SceneLoad.reset();
		m_SceneLoadStatus.State = SceneLoadState::Cancelled;
		m_SceneLoadStatus.Error = "Opening the scene was cancelled";
		m_SceneLoadStatus.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_SceneLoadStatus.Started).count();
	}

	void EditorContext::UpdateSceneLoad(SceneWorkBudget* budget)
	{
		if (!m_SceneLoad)
			return;
		ST_PROFILE_STAGE("Scene.Load");
		SceneLoadRequest& request = m_SceneLoad->Request;
		request.Update(budget);
		m_SceneLoadStatus.State = request.GetState();
		m_SceneLoadStatus.Progress = request.GetProgress();
		if (!request.IsFinished())
			return;

		m_SceneLoadStatus.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_SceneLoadStatus.Started).count();
		const AssetHandle handle = request.GetSceneAsset();
		const bool opened = request.GetState() == SceneLoadState::Ready;
		if (!opened)
		{
			m_SceneLoadStatus.Error = request.GetError();
			// scene.open reports its own failures; the start scene's has nobody else to tell (the project stays open).
			if (m_SceneLoadStatus.StartScene)
				ST_WARN("Could not open the start scene: {}", m_SceneLoadStatus.Error);
			m_SceneLoad.reset();
			return;
		}

		// The command that started the load no longer runs between frames: the replacement is its work all the same, as a poll
		// of it would be. So the person hears who discarded unsaved changes (NotifyDiscardedChanges), and the prefab catch-up's
		// undo step is the command's, which a replay of it repeats, not an edit outside commands.
		const uint64_t requestedBy = m_SceneLoad->RequestedBy;
		const uint64_t records = m_UndoStack.GetRecordCount();
		if (requestedBy != 0)
			m_Journal->NoteEditsOutsideCommands(m_UndoStack);
		const bool resumed = m_Journal->Resume(requestedBy);
		// The scene is in place before the request (and its pins) goes, so what it uses is requested from now on.
		FinishOpenScene(request.TakeScene(), handle, request.GetAssets());
		m_SceneLoad.reset();
		// Once the scene is editable: instances of prefabs that changed while it was closed take their values (an undo step)
		// before anything else, scene.open's answer included, sees the scene.
		m_PrefabSync.OnSceneOpened(*this);
		if (resumed)
			m_Journal->Suspend(requestedBy);
		if (requestedBy != 0)
			m_Journal->NoteUndo(requestedBy, m_UndoStack, records);
	}

	void EditorContext::FinishOpenScene(Ref<Scene> scene, AssetHandle handle, std::span<const AssetHandle> loaded)
	{
		ResetScene(std::move(scene), handle);
		// What the previous scene used, this one's load did not bring in and the viewport does not request for this one in its
		// first frames is released then.
		m_AssetManager->ScheduleTrim(AssetResidency::c_SceneSwitchTrimFrames, AssetResidency::c_SceneSwitchTrimFrames, loaded);
		if (!m_Viewport.RestoreSceneCamera(handle))
		{
			// Shown for the first time: frame what it renders. Meshes that are still loading stand in as placeholder boxes,
			// so the view is framed again once they are known.
			std::vector<AssetHandle> pendingMeshes;
			if (m_Viewport.FrameScene(*this, &pendingMeshes) && !pendingMeshes.empty())
			{
				m_PendingFrame = PendingFrame { handle, std::move(pendingMeshes), m_Viewport.GetCamera(),
					std::chrono::steady_clock::now() + c_PendingFrameTimeout };
			}
		}
	}

	void EditorContext::UpdatePendingFrame()
	{
		if (!m_PendingFrame)
			return;
		// Only the first view of the scene is framed again, and never once the user moved the camera.
		if (m_PendingFrame->Scene != m_SceneHandle || IsPlaying() || !(m_Viewport.GetCamera() == m_PendingFrame->FramedCamera)
			|| std::chrono::steady_clock::now() > m_PendingFrame->Deadline)
		{
			m_PendingFrame.reset();
			return;
		}
		const bool loading = std::any_of(m_PendingFrame->Meshes.begin(), m_PendingFrame->Meshes.end(),
			[](AssetHandle mesh) { return AssetManager::GetAssetState(mesh) == AssetState::Loading; });
		if (loading)
			return;
		m_PendingFrame.reset();
		m_Viewport.FrameScene(*this);
	}

	uint64_t EditorContext::BeginSaveScene(std::string* outError)
	{
		if (!m_AssetManager)
		{
			if (outError)
				*outError = "No project is open";
			return 0;
		}
		if (!m_SceneHandle.IsValid())
		{
			if (outError)
				*outError = "The scene has not been saved yet; save it under a path first";
			return 0;
		}
		// A world saves the documents it changed: the persistent scene, the dirty cells, its index.
		if (m_World)
			return SubmitWorldSave(outError);
		return SubmitSceneSave(m_SceneHandle, {}, std::nullopt, outError);
	}

	uint64_t EditorContext::BeginSaveSceneAs(const std::string& relativePath, std::string* outError)
	{
		if (!m_AssetManager)
		{
			if (outError)
				*outError = "No project is open";
			return 0;
		}
		if (m_World)
		{
			if (outError)
				*outError = "A world is saved in place (scene.save); world.create makes another";
			return 0;
		}
		const std::filesystem::path path = FileSystem::FromUTF8(relativePath);
		const AssetImporter* importer = AssetImporterRegistry::FindByExtension(StringUtils::ToLower(FileSystem::ToUTF8(path.extension())));
		if (!importer || importer->GetType() != AssetType::Scene)
		{
			if (outError)
				*outError = fmt::format("'{}' is not a scene file (.stscene)", relativePath);
			return 0;
		}
		const AssetHandle handle = m_AssetManager->ReserveNativeAsset(relativePath, UUID::Null(), outError);
		if (!handle.IsValid())
			return 0;

		// The scene is the new asset from now on (a following save writes it) and takes the file's name; a failed save gives
		// the previous ones back.
		const SceneSaveRestore restore { m_SceneHandle, m_EditScene->GetName(), m_EditScene->GetSourceAsset() };
		m_EditScene->SetName(FileSystem::ToUTF8(path.stem()));
		m_SceneHandle = handle;
		m_EditScene->SetSourceAsset(handle);
		const uint64_t id = SubmitSceneSave(handle, m_AssetManager->GetReservedPath(handle), restore, outError);
		if (id == 0)
		{
			m_SceneHandle = restore.Handle;
			m_EditScene->SetName(restore.Name);
			m_EditScene->SetSourceAsset(restore.SourceAsset);
			m_AssetManager->ReleaseNativeAssetReservation(handle);
		}
		return id;
	}

	uint64_t EditorContext::SubmitSceneSave(AssetHandle handle, const std::string& newPath, std::optional<SceneSaveRestore> restore, std::string* outError)
	{
		// The snapshot holds the history's state from here on: a drag that goes on makes a step of its own.
		m_UndoStack.BreakMerge();
		DocumentSaveRequest request;
		request.SavePoint = m_UndoStack.GetSavePoint();
		request.Snapshot = Scene::Copy(m_EditScene, SceneCopyOptions { true });
		request.Label = m_EditScene->GetName();
		DocumentSaveItem item;
		item.Asset = handle;
		item.NewPath = newPath;
		request.Items.push_back(std::move(item));
		const SceneRecovery::SaveToken token = m_Recovery->BeginSave();
		const uint64_t id = m_DocumentSaver.Submit(std::move(request), [this, handle, token, restore](const DocumentSaveStatus& status)
		{
			OnSceneSaveEnded(status, handle, token, restore);
		}, outError);
		if (id == 0)
			m_Recovery->ReleaseSave(token);
		return id;
	}

	void EditorContext::OnSceneSaveEnded(const DocumentSaveStatus& status, AssetHandle handle, const RecoverySaveToken& token,
		const std::optional<SceneSaveRestore>& restore)
	{
		// The edited scene was replaced meanwhile (another scene or project): the file is written, but nothing of the scene now
		// edited is marked, and its journal hears nothing (MarkSaved and OnSaved recognize another scene's point and token).
		const bool sameScene = status.SavePoint.Generation == m_UndoStack.GetSavePoint().Generation;
		// A save as a new asset that failed gives the scene its previous handle and name back, also when a newer save of the
		// new asset replaced it before it began (that one's completion has nothing to give back).
		if (status.State != DocumentSaveState::Done && sameScene && restore && m_SceneHandle == handle)
		{
			m_SceneHandle = restore->Handle;
			m_EditScene->SetName(restore->Name);
			m_EditScene->SetSourceAsset(restore->SourceAsset);
		}
		// A newer save of the same scene replaced this one before it began: the newer one's completion does the rest.
		if (status.WrittenID != status.ID)
		{
			m_Recovery->ReleaseSave(token);
			return;
		}
		if (status.State != DocumentSaveState::Done)
		{
			m_Recovery->ReleaseSave(token);
			NotifySaveFailed(status.Label, status.Error, handle);
			return;
		}
		if (!sameScene)
		{
			m_Recovery->ReleaseSave(token);
			return;
		}
		m_UndoStack.MarkSaved(status.SavePoint);
		OnSceneSaved();
		const auto [sceneFile, scenePath] = GetSceneFile(handle);
		const std::string baseHash = status.Items.empty() ? std::string() : Crypto::ToHex(status.Items.front().Sha256);
		m_Recovery->OnSaved(handle, sceneFile, scenePath, baseHash, token, *m_EditScene, IsSceneModified());
	}

	void EditorContext::NotifySaveFailed(const std::string& label, const std::string& error, AssetHandle asset)
	{
		EditorNotification notification;
		notification.Level = NotificationLevel::Error;
		notification.Title = fmt::format("Saving '{}' failed", label);
		notification.Message = error;
		notification.ShowConsole = true;
		if (m_AssetManager && m_AssetManager->IsHandleValid(asset))
			notification.Asset = asset;
		m_Notifications.Post(std::move(notification));
	}

	uint64_t EditorContext::SubmitWorldSave(std::string* outError)
	{
		// The snapshot holds the history's state from here on: a drag that goes on makes a step of its own.
		m_UndoStack.BreakMerge();
		Ref<WorldSavePlan> plan = CreateRef<WorldSavePlan>();
		if (!m_World->BeginSave(*m_AssetManager, m_SceneHandle, *plan, outError))
			return 0;
		DocumentSaveRequest request;
		request.SavePoint = m_UndoStack.GetSavePoint();
		request.Snapshot = Scene::Copy(m_EditScene, SceneCopyOptions { true });
		request.Label = m_EditScene->GetName();
		request.Items = plan->Items;
		const SceneRecovery::SaveToken token = m_Recovery->BeginSave();
		const AssetHandle handle = m_SceneHandle;
		const uint64_t id = m_DocumentSaver.Submit(std::move(request), [this, handle, token, plan](const DocumentSaveStatus& status)
		{
			OnWorldSaveEnded(status, handle, token, *plan);
		}, outError);
		if (id == 0)
		{
			m_Recovery->ReleaseSave(token);
			m_World->CancelSave(*m_AssetManager, *plan);
			return 0;
		}
		m_WorldSave = WorldSaveState { id, false, {}, {} };
		return id;
	}

	void EditorContext::OnWorldSaveEnded(const DocumentSaveStatus& status, AssetHandle handle, const RecoverySaveToken& token, const WorldSavePlan& plan)
	{
		std::vector<WorldSavedDocument> documents;
		for (size_t index = 0; index < plan.Documents.size() && index < status.Items.size(); index++)
		{
			const DocumentSaveItemResult& item = status.Items[index];
			if (item.Error.empty())
				documents.push_back({ item.Path.empty() ? plan.Documents[index].Path : item.Path, plan.Documents[index].Kind, item.Bytes, item.Entities });
		}
		const bool current = m_WorldSave.Request == status.ID;
		if (current)
			m_WorldSave.Documents = documents;
		// The world was replaced meanwhile (another scene or project): the files are written, but nothing of what is edited
		// now is marked, and its journal hears nothing.
		const bool sameWorld = m_World && m_SceneHandle == handle && status.SavePoint.Generation == m_UndoStack.GetSavePoint().Generation;
		if (!sameWorld || !m_AssetManager)
		{
			m_Recovery->ReleaseSave(token);
			if (current)
				m_WorldSave.Ended = true;
			return;
		}
		std::string indexError;
		const std::optional<std::string> index = m_World->FinishSave(*m_AssetManager, plan, status, &indexError);
		const bool written = status.State == DocumentSaveState::Done;
		// The journal follows the persistent scene's document: the one written now, or the file as it is.
		std::string baseHash;
		if (written)
		{
			const auto persistent = std::find_if(plan.Documents.begin(), plan.Documents.end(), [](const WorldSavePlan::Document& document) { return document.Kind == "persistent"; });
			if (persistent != plan.Documents.end())
			{
				baseHash = Crypto::ToHex(status.Items[static_cast<size_t>(persistent - plan.Documents.begin())].Sha256);
			}
			else
			{
				std::vector<uint8_t> bytes;
				if (m_AssetManager->ReadStoredData(handle, bytes))
					baseHash = Crypto::ToHex(Sha256::Hash(std::span<const uint8_t>(bytes)));
			}
		}
		else
		{
			m_Recovery->ReleaseSave(token);
			NotifySaveFailed(status.Label, status.Error, handle);
		}
		if (!index)
		{
			if (!indexError.empty())
			{
				// The cells are written, but the world cannot list them: the scene stays modified, and the next save tries again.
				NotifySaveFailed(status.Label, indexError, m_World->GetWorld());
				if (written)
					m_Recovery->ReleaseSave(token);
				if (current)
					m_WorldSave.Error = indexError;
			}
			else if (written)
			{
				MarkWorldSaved(status.SavePoint, handle, token, std::move(baseHash));
			}
			if (current)
				m_WorldSave.Ended = true;
			return;
		}
		// The World document with the saved cells' index entries, written once their files are (their stamps are known now).
		DocumentSaveRequest request;
		DocumentSaveItem item;
		item.Asset = m_World->GetWorld();
		item.Document = *index;
		item.Reload = true;
		request.Items.push_back(std::move(item));
		request.SavePoint = status.SavePoint;
		request.Label = fmt::format("{} (index)", status.Label);
		std::string error;
		const std::optional<RecoverySaveToken> heldToken = written ? std::optional<RecoverySaveToken>(token) : std::nullopt;
		if (m_DocumentSaver.Submit(std::move(request), [this, id = status.ID, handle, heldToken, baseHash](const DocumentSaveStatus& indexStatus)
		{
			OnWorldIndexSaved(indexStatus, id, handle, heldToken ? &*heldToken : nullptr, baseHash);
		}, &error) == 0)
		{
			m_World->FinishIndexSave();
			NotifySaveFailed(status.Label, error, m_World->GetWorld());
			if (written)
				m_Recovery->ReleaseSave(token);
			if (current)
			{
				m_WorldSave.Error = error;
				m_WorldSave.Ended = true;
			}
		}
	}

	void EditorContext::OnWorldIndexSaved(const DocumentSaveStatus& status, uint64_t save, AssetHandle handle, const RecoverySaveToken* token, std::string baseHash)
	{
		const bool current = m_WorldSave.Request == save;
		const bool indexWritten = status.State == DocumentSaveState::Done && !status.Items.empty();
		if (!indexWritten)
			NotifySaveFailed(status.Label, status.Error, status.Items.empty() ? UUID::Null() : status.Items.front().Asset);
		// Another world (or none) is edited now: the files are written, but nothing of what is edited now is marked.
		const bool sameWorld = m_World && m_SceneHandle == handle && status.SavePoint.Generation == m_UndoStack.GetSavePoint().Generation;
		if (current && m_World)
			m_World->FinishIndexSave();
		if (token)
		{
			if (indexWritten && sameWorld)
				MarkWorldSaved(status.SavePoint, handle, *token, std::move(baseHash));
			else
				m_Recovery->ReleaseSave(*token);
		}
		if (!current)
			return;
		if (indexWritten)
			m_WorldSave.Documents.push_back({ status.Items.front().Path, "world", status.Items.front().Bytes, 0 });
		else
			m_WorldSave.Error = status.Error.empty() ? std::string("The world's document was not written") : status.Error;
		m_WorldSave.Ended = true;
	}

	void EditorContext::MarkWorldSaved(const UndoStack::SavePoint& savePoint, AssetHandle handle, const RecoverySaveToken& token, std::string baseHash)
	{
		m_UndoStack.MarkSaved(savePoint);
		OnSceneSaved();
		const auto [sceneFile, scenePath] = GetSceneFile(handle);
		m_Recovery->OnSaved(handle, sceneFile, scenePath, std::move(baseHash), token, *m_EditScene, IsSceneModified());
	}

	std::optional<std::vector<WorldSavedDocument>> EditorContext::GetWorldSaveDocuments(uint64_t save) const
	{
		if (save == 0 || m_WorldSave.Request != save || !m_WorldSave.Ended)
			return std::nullopt;
		return m_WorldSave.Documents;
	}

	std::string EditorContext::GetWorldSaveError(uint64_t save) const
	{
		if (save == 0 || m_WorldSave.Request != save || !m_WorldSave.Ended)
			return std::string();
		return m_WorldSave.Error;
	}

	bool EditorContext::WaitForSave(uint64_t id, std::string* outError)
	{
		m_DocumentSaver.FinishAll();
		const std::optional<DocumentSaveStatus> status = m_DocumentSaver.GetStatus(id);
		if (!status || status->State != DocumentSaveState::Done)
		{
			if (outError)
				*outError = status ? status->Error : std::string("The save ended without a status");
			return false;
		}
		return true;
	}

	uint64_t EditorContext::FindSaveOfCurrentState() const
	{
		const UndoStack::SavePoint current = m_UndoStack.GetSavePoint();
		for (const DocumentSaveStatus& save : m_DocumentSaver.GetActiveStatuses())
		{
			if (save.SavePoint == current)
				return save.ID;
		}
		return 0;
	}

	bool EditorContext::SaveScene(std::string* outError)
	{
		const uint64_t id = BeginSaveScene(outError);
		return id != 0 && WaitForSave(id, outError);
	}

	bool EditorContext::SaveSceneAs(const std::string& relativePath, std::string* outError)
	{
		const uint64_t id = BeginSaveSceneAs(relativePath, outError);
		return id != 0 && WaitForSave(id, outError);
	}

	void EditorContext::OnSceneSaved()
	{
		m_ProjectThumbnail->Request();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Play mode
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::Play(std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (m_ScriptEngine)
		{
			if (std::optional<ScriptFault> fault = m_ScriptEngine->GetFault())
			{
				return fail(fmt::format("The script module crashed earlier ({}: {}); it stays disabled until it is rebuilt (script.build) or reloaded "
					"(script.reload)", fault->ClassName.empty() ? fault->ModuleName : fault->ClassName + "::" + fault->Method, fault->Description));
			}
			// Scenes use the engine that is active when they start.
			ScriptEngine::SetActive(m_ScriptEngine);
		}
		if (!StartRuntime(SceneRuntimeMode::Play, outError))
			return false;
		// A crash while the scripts start (constructors, OnCreate) ends play mode right away.
		if (StopOnScriptFault())
			return fail("A script crashed while the scene started; play mode was stopped (see the log or script.status)");
		return true;
	}

	bool EditorContext::Simulate(std::string* outError)
	{
		return StartRuntime(SceneRuntimeMode::Simulate, outError);
	}

	bool EditorContext::StartRuntime(SceneRuntimeMode mode, std::string* outError)
	{
		if (IsPlaying())
		{
			if (outError)
				*outError = fmt::format("The scene is already running ({})", SceneStateToString(m_SceneState));
			return false;
		}
		if (IsSceneLoading())
		{
			if (outError)
			{
				*outError = fmt::format("A scene is being opened ({}, {:.0f}%): play once it is open (scene.open answers then; scene.loadStatus)",
					m_SceneLoadStatus.Scene.ToString(), m_SceneLoadStatus.Progress * 100.0f);
			}
			return false;
		}
		m_UndoStack.BreakMerge();
		m_MasterVolumeBeforePlay = AudioEngine::GetMasterVolume();
		m_RuntimeScene = Scene::Copy(m_EditScene);
		// A world plays with the cells the editor has loaded (its edit radius), never the whole world: the game streams the
		// others around the play camera from their saved documents.
		const std::vector<WorldCellKey> pinnedCells = m_World ? m_World->PrepareRuntimeCopy(*m_RuntimeScene) : std::vector<WorldCellKey>();
		m_RuntimeSceneAsset = UUID::Null();
		m_PlaySession++;
		m_ReplacedFixedSteps = 0;
		m_FrozenInputFrames = 0;
		// Keys a tool held down in an earlier session (input.* commands) must not leak into this one.
		ResetSimulatedInput();
		ResetGameShell();
		m_RuntimeScene->SetTimeDilation(m_Specification.TimeDilation);
		m_RuntimeScene->OnRuntimeStart(mode);
		// Cells with unsaved edits stay for the session: their documents on disk are older than what plays.
		if (WorldStreamingSystem* streaming = m_RuntimeScene->GetSystem<WorldStreamingSystem>())
		{
			for (const WorldCellKey& key : pinnedCells)
				streaming->PinCell(key);
		}
		m_SceneState = mode == SceneRuntimeMode::Play ? SceneState::Play : SceneState::Simulate;
		UpdateInputSuspension();
		return true;
	}

	void EditorContext::Stop()
	{
		if (!m_RuntimeScene)
			return;
		if (m_RuntimeSceneLoad)
		{
			m_RuntimeSceneLoad->Request.Cancel();
			m_RuntimeSceneLoad.reset();
		}
		m_FixedStepsAtStop = GetFixedStepCount();
		m_StoppedSession = m_PlaySession;
		// Stopped here and destroyed off the frame, like a replaced scene (ReplaceRuntimeScene).
		Scene::ReleaseInBackground(std::move(m_RuntimeScene));
		m_RuntimeSceneAsset = UUID::Null();
		m_SceneState = SceneState::Edit;
		// A game's volume setting belongs to the game session, not to the editor.
		AudioEngine::SetMasterVolume(m_MasterVolumeBeforePlay);
		m_GameInputActive = false;
		ResetSimulatedInput(); // The game that a tool's simulated input was meant for is gone
		ResetGameShell();
		UpdateInputSuspension();
		PruneSelection(); // Entities created during play are gone
	}

	void EditorContext::HandleRuntimeRequests()
	{
		if (const std::optional<int32_t> exitCode = m_RuntimeScene->GetQuitRequest())
		{
			ST_INFO("The game quit with exit code {}; play mode stopped", *exitCode);
			Stop();
			return;
		}

		const std::optional<UUID> request = m_RuntimeScene->TakeSceneLoadRequest();
		std::string error;
		if (request && !SwitchRuntimeScene(*request, &error))
			ST_ERROR("The game cannot switch scenes: {}", error);
	}

	bool EditorContext::SwitchRuntimeScene(AssetHandle scene, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		// The null handle restarts what runs: the scene asset switched to last, or else the edited scene (unsaved changes
		// included, as play mode started with them), which needs no loading.
		const AssetHandle target = scene.IsValid() ? scene : m_RuntimeSceneAsset;
		if (!target.IsValid())
		{
			if (m_RuntimeSceneLoad)
			{
				m_RuntimeSceneLoad->Request.Cancel();
				m_RuntimeSceneLoad.reset();
				m_RuntimeScene->SetSceneLoadProgress(std::nullopt);
			}
			ST_PROFILE_STAGE("Scene.Load");
			ReplaceRuntimeScene(Scene::Copy(m_EditScene), UUID::Null(), {});
			return true;
		}
		if (!m_AssetManager || m_AssetManager->GetAssetType(target) != AssetType::Scene)
			return fail(fmt::format("{} is not a scene asset of the project", target.ToString()));

		// Like the exported game (GameRuntime::LoadScene): the running scene goes on while the next one and everything it
		// uses load, then UpdateRuntimeSceneLoad swaps it in. Asking again for the scene that is loading keeps its load
		// (scripts ask in every update while their condition holds); a request for another scene replaces it, starting
		// first so that what both use keeps loading (the replaced request's Cancel leaves assets others pin alone).
		if (m_RuntimeSceneLoad && m_RuntimeSceneLoad->Scene == target)
			return true;
		Scope<RuntimeSceneLoad> next = CreateScope<RuntimeSceneLoad>();
		next->Scene = target;
		SceneLoadOptions options;
		options.Prepare = GetScenePreparation();
		// The scene will run: its systems prepare it too (physics' bodies), so that the swap builds nothing.
		options.PrepareSystems = true;
		next->Request = SceneLoader::LoadAsync(target, options);
		m_RuntimeSceneLoad = std::move(next);
		m_RuntimeScene->SetSceneLoadProgress(0.0f);
		return true;
	}

	void EditorContext::UpdateRuntimeSceneLoad(SceneWorkBudget& budget)
	{
		if (!m_RuntimeSceneLoad || !m_RuntimeScene)
			return;
		ST_PROFILE_STAGE("Scene.Load");
		SceneLoadRequest& request = m_RuntimeSceneLoad->Request;
		request.Update(&budget);
		m_RuntimeScene->SetSceneLoadProgress(request.GetProgress());
		if (!request.IsFinished())
			return;
		if (request.GetState() == SceneLoadState::Ready)
			ReplaceRuntimeScene(request.TakeScene(), m_RuntimeSceneLoad->Scene, request.GetAssets());
		else
			ST_ERROR("The game cannot switch scenes: {}", request.GetError());
		m_RuntimeScene->SetSceneLoadProgress(std::nullopt);
		m_RuntimeSceneLoad.reset();
	}

	void EditorContext::ReplaceRuntimeScene(Ref<Scene> next, AssetHandle asset, std::span<const AssetHandle> loaded)
	{
		// The new scene continues the way play mode was going: paused (with the steps still to run) or running.
		const SceneRuntimeMode mode = m_RuntimeScene->GetRuntimeMode();
		const bool paused = m_RuntimeScene->IsPaused();
		const uint32_t steps = m_RuntimeScene->GetStepFrames();
		m_ReplacedFixedSteps += m_RuntimeScene->GetFixedStepCount();
		// Said in the log and the activity journal: commands and agents would otherwise think the edited scene still runs.
		auto describe = [this](AssetHandle scene, const std::string& name) -> nlohmann::json
		{
			const std::optional<AssetMetadata> metadata = scene.IsValid() && m_AssetManager ? m_AssetManager->GetMetadata(scene) : std::nullopt;
			return { { "scene", scene.IsValid() ? UUIDToJson(scene) : nlohmann::json(nullptr) }, { "name", name },
				{ "path", metadata ? nlohmann::json(metadata->Path) : nlohmann::json(nullptr) } };
		};
		const nlohmann::json from = describe(m_RuntimeSceneAsset, m_RuntimeScene->GetName());
		const nlohmann::json to = describe(asset, next->GetName());
		if (to["path"].is_string())
			ST_INFO("Play mode switched to '{}' ({})", next->GetName(), to["path"].get<std::string>());
		else
			ST_INFO("Play mode switched to '{}'", next->GetName());
		m_Journal->RecordEvent("play.sceneSwitched", { { "from", from }, { "to", to } }, true);
		// The scene that goes stops here and is destroyed off the frame (a million entities take tens of milliseconds).
		Scene::ReleaseInBackground(std::move(m_RuntimeScene));
		m_RuntimeScene = std::move(next);
		m_RuntimeSceneAsset = asset;
		AdoptProjectLayers(*m_RuntimeScene, asset);
		m_RuntimeScene->SetTimeDilation(m_Specification.TimeDilation);
		m_RuntimeScene->OnRuntimeStart(mode);
		if (paused)
		{
			m_RuntimeScene->SetPaused(true);
			m_RuntimeScene->Step(steps);
		}
		PruneSelection(); // The selection named entities of the previous scene
		UpdateInputSuspension();
		// Like the exported game (GameRuntime): what the previous scene used, this one's load did not bring in and this one does
		// not request in its first frames is released then, so playtests show the game's memory behavior.
		if (m_AssetManager)
			m_AssetManager->ScheduleTrim(AssetResidency::c_SceneSwitchTrimFrames, AssetResidency::c_SceneSwitchTrimFrames, loaded);
	}

	void EditorContext::SetGameInputActive(bool active)
	{
		m_GameInputActive = active && m_SceneState == SceneState::Play;
	}

	void EditorContext::SetPaused(bool paused)
	{
		if (!paused && IsPaused())
		{
			m_FixedStepsAtResume = GetFixedStepCount();
			m_ResumeCount++;
		}
		if (m_RuntimeScene)
			m_RuntimeScene->SetPaused(paused);
		UpdateInputSuspension();
	}

	bool EditorContext::IsPaused() const
	{
		return m_RuntimeScene && m_RuntimeScene->IsPaused();
	}

	void EditorContext::Step(uint32_t frames)
	{
		if (m_RuntimeScene && m_RuntimeScene->IsPaused())
			m_RuntimeScene->Step(frames);
		UpdateInputSuspension();
	}

	uint64_t EditorContext::StartInputSequence(Scope<InputSequence> sequence, std::string* outError)
	{
		if (IsInputSequenceRunning())
		{
			if (outError)
				*outError = "An input sequence is running: it steps the game until it ends";
			return 0;
		}
		if (HasPendingSteps())
		{
			if (outError)
				*outError = "play.step's steps are still running: wait for them (or resume the game) first";
			return 0;
		}
		m_InputSequence = std::move(sequence);
		m_InputSequenceId++;
		if (!m_InputSequence->Start(*this, outError))
			return 0;
		return m_InputSequenceId;
	}

	bool EditorContext::IsInputSequenceRunning() const
	{
		return m_InputSequence && !m_InputSequence->IsFinished();
	}

	uint32_t EditorContext::GetPendingSteps() const
	{
		return m_RuntimeScene && m_RuntimeScene->IsPaused() ? m_RuntimeScene->GetStepFrames() : 0;
	}

	uint64_t EditorContext::GetFixedStepCount() const
	{
		return m_RuntimeScene ? m_ReplacedFixedSteps + m_RuntimeScene->GetFixedStepCount() : 0;
	}

	void EditorContext::UpdateInputSuspension()
	{
		// The game updates in the next frame unless it is paused without a step to run: only then may input frames go on.
		Input::SetSuspended(m_RuntimeScene && m_RuntimeScene->IsPaused() && m_RuntimeScene->GetStepFrames() == 0);
	}

	void EditorContext::ResetSimulatedInput()
	{
		Input::ClearSimulated();
		m_SimulatedInput.Reset();
	}

	void EditorContext::ResetGameShell()
	{
		m_GameWindow.Clear();
		Input::SetGameCursorMode(CursorMode::Normal);
	}

	std::optional<std::filesystem::path> EditorContext::GetGameSaveDirectory() const
	{
		if (!m_Project)
			return std::nullopt;
		return m_Project->GetIntermediateDirectory() / "Saves";
	}

	////////////////////////////////////////////////////////////////////////////////
	// Selection
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::SetSelection(std::vector<UUID> selection)
	{
		ClearSelection();
		for (UUID entity : selection)
		{
			if (GetActiveScene()->GetEntityByUUID(entity) && m_SelectionSet.insert(entity).second)
				m_Selection.push_back(entity);
		}
		if (!m_Selection.empty())
		{
			m_SelectionVersion++;
			InspectEntities();
		}
	}

	void EditorContext::Select(UUID entity, bool additive)
	{
		if (!additive)
			ClearSelection();
		if (!GetActiveScene()->GetEntityByUUID(entity))
			return;
		// The latest selection is the primary one: move it to the back.
		if (!m_SelectionSet.insert(entity).second)
			std::erase(m_Selection, entity);
		m_Selection.push_back(entity);
		m_SelectionVersion++;
		InspectEntities();
	}

	void EditorContext::Deselect(UUID entity)
	{
		if (m_SelectionSet.erase(entity) > 0)
		{
			std::erase(m_Selection, entity);
			m_SelectionVersion++;
		}
	}

	void EditorContext::ClearSelection()
	{
		if (m_Selection.empty())
			return;
		m_Selection.clear();
		m_SelectionSet.clear();
		m_SelectionVersion++;
	}

	bool EditorContext::IsSelected(UUID entity) const
	{
		return m_SelectionSet.contains(entity);
	}

	Entity EditorContext::GetPrimarySelection() const
	{
		for (auto it = m_Selection.rbegin(); it != m_Selection.rend(); ++it)
		{
			if (Entity entity = GetActiveScene()->GetEntityByUUID(*it))
				return entity;
		}
		return {};
	}

	void EditorContext::PruneSelection()
	{
		if (std::erase_if(m_Selection, [this](UUID entity)
		{
			if (GetActiveScene()->GetEntityByUUID(entity))
				return false;
			m_SelectionSet.erase(entity);
			return true;
		}) > 0)
			m_SelectionVersion++;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Inspection
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::SetInspectedAsset(AssetHandle handle)
	{
		if (!handle.IsValid())
		{
			InspectEntities();
			return true;
		}
		if (!AssetManager::IsHandleValid(handle))
			return false;
		// Every request is a new one, also for the asset shown already: a click on it brings its tab back after the user
		// looked at another one.
		m_InspectedAsset = handle;
		m_InspectionVersion++;
		return true;
	}

	void EditorContext::InspectEntities()
	{
		if (!m_InspectedAsset.IsValid())
			return;
		m_InspectedAsset = UUID::Null();
		m_InspectionVersion++;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Undo
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::CommitEdit(SceneEditTransaction& transaction, OverrideUpkeep overrides)
	{
		if (IsPlaying())
			return false;
		if (IsSceneLoading())
		{
			// The edited scene is about to be replaced: an edit now would be lost with it, so it is undone at once.
			transaction.Rollback();
			return false;
		}
		// In a world, entities move into the cells their positions lie in, within the same step.
		if (m_World)
			m_World->Recell(transaction);
		if (overrides == OverrideUpkeep::Update)
			UpdatePrefabOverrides(transaction);
		return transaction.Commit(m_UndoStack);
	}

	void EditorContext::UpdatePrefabOverrides(SceneEditTransaction& transaction)
	{
		Scene& scene = transaction.GetScene();
		if (!m_AssetManager || &scene != m_EditScene.get())
			return;
		// A scene without prefab links has no instances to look for (edits of large scenes without prefabs pay nothing).
		const auto* links = std::as_const(scene.GetRegistry()).storage<PrefabInstanceComponent>();
		if (!links || links->empty())
			return;

		// The instances the edit touched, with the entities it changed in each. An entity added to an instance (not linked)
		// belongs to the instance of its nearest linked ancestor.
		struct TouchedInstance
		{
			Entity Root;
			std::vector<Entity> Entities; // Entities whose components alone changed
			bool Reshaped = false;        // Entities were made, destroyed, moved or linked otherwise: the whole list is worked out
		};
		std::vector<TouchedInstance> instances;
		std::unordered_map<UUID, size_t> instanceOf;
		auto touch = [&](Entity entity) -> TouchedInstance*
		{
			while (entity && !entity.HasComponent<PrefabInstanceComponent>())
				entity = entity.GetParent();
			const Entity root = entity ? PrefabInstances::FindInstanceRoot(entity) : Entity();
			if (!root)
				return nullptr;
			const auto [found, added] = instanceOf.try_emplace(root.GetUUID(), instances.size());
			if (added)
				instances.push_back({ root });
			return &instances[found->second];
		};
		// Whether the entity has the link it had before the edit (the same prefab and prefab entity).
		auto sameLink = [](const EntityState& state, Entity entity)
		{
			const nlohmann::json* before = JsonUtils::Find(state.Components, "PrefabInstance");
			const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
			if (!before || !link)
				return !before && !link;
			const nlohmann::json* prefab = JsonUtils::Find(*before, "Prefab");
			const nlohmann::json* id = JsonUtils::Find(*before, "PrefabEntityID");
			return prefab && id && id->is_string() && UUIDFromJson(*prefab) == std::optional<UUID>(link->Prefab)
				&& id->get_ref<const std::string&>() == link->PrefabEntityID.ToString();
		};
		for (const EntityState& state : transaction.GetBefore())
		{
			const Entity entity = scene.GetEntityByUUID(state.ID);
			const Entity parent = entity ? entity.GetParent() : Entity();
			const bool reshaped = !state.Exists || !entity || (parent ? parent.GetUUID() : UUID::Null()) != state.Parent || !sameLink(state, entity);
			if (TouchedInstance* instance = touch(entity))
			{
				if (reshaped)
					instance->Reshaped = true;
				else
					instance->Entities.push_back(entity);
			}
			// The instance it left, or was destroyed in.
			if (reshaped && state.Parent.IsValid())
			{
				if (TouchedInstance* instance = touch(scene.GetEntityByUUID(state.Parent)))
					instance->Reshaped = true;
			}
		}

		// Each against its part of the prefab as the sync knows it; the list changes with the edit (and undo restores it).
		for (const TouchedInstance& instance : instances)
		{
			Entity root = instance.Root;
			// Tracked whether its list changes or not: the steps of a gizmo drag or a slider then all have the same entities and
			// merge into one, also when only some of them change the list.
			transaction.Track(root.GetUUID());
			const PrefabInstanceComponent& link = root.GetComponent<PrefabInstanceComponent>();
			const PrefabOverrides::IndexedPart* part = m_PrefabSync.GetBaselinePart(*this, link.Prefab, link.PrefabEntityID);
			if (!part)
				continue;
			std::vector<PrefabOverride> overrides = link.Overrides;
			if (!instance.Reshaped && PrefabOverrides::UpdateOverrides(root, *part, instance.Entities, overrides))
			{
				m_OverrideUpkeep.Entities += instance.Entities.size();
			}
			else
			{
				overrides = PrefabOverrides::ComputeOverrides(scene, root, part->Part);
				m_OverrideUpkeep.WholeLists++;
			}
			if (overrides != link.Overrides)
				root.GetComponent<PrefabInstanceComponent>().Overrides = std::move(overrides);
		}
	}

	bool EditorContext::Undo(std::string* outError)
	{
		if (IsPlaying())
		{
			if (outError)
				*outError = "Undo is not available while the scene is running";
			return false;
		}
		// While a scene is being opened the edited scene, and its history, are read-only.
		if (!CheckSceneEditable(outError))
			return false;
		const bool undone = m_UndoStack.Undo(outError);
		PruneSelection();
		return undone;
	}

	bool EditorContext::Redo(std::string* outError)
	{
		if (IsPlaying())
		{
			if (outError)
				*outError = "Redo is not available while the scene is running";
			return false;
		}
		if (!CheckSceneEditable(outError))
			return false;
		const bool redone = m_UndoStack.Redo(outError);
		PruneSelection();
		return redone;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Recovery
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::BeginRecovery(const RecoveryData& data, bool force, uint64_t* outSceneLoad, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		*outSceneLoad = 0;
		const RecoveryEntry& entry = data.Entry;
		if (!m_AssetManager)
			return fail("No project is open");
		if (IsPlaying())
			return fail("Stop play mode first (play.stop): recovering replaces the edited scene");
		// The open under way would replace whatever is recovered now.
		if (!CheckSceneEditable(outError))
			return false;
		const bool modified = IsSceneModified();
		const bool replaces = !entry.Scene.IsValid() || m_SceneHandle != entry.Scene || modified;
		if (replaces && modified && !force)
		{
			return fail(fmt::format("The scene '{}' has unsaved changes, which recovering '{}' would discard: save them first or pass force: true",
				m_EditScene->GetName(), entry.SceneName));
		}
		if (!replaces)
			return true;
		if (entry.Scene.IsValid())
		{
			// The saved scene the changes apply to, as its file holds it, opened like scene.open does: never in one frame.
			if (!BeginOpenScene(entry.Scene, outError))
				return false;
			*outSceneLoad = m_SceneLoadStatus.ID;
			return true;
		}
		// A scene that was never saved: its complete journal fills a new, empty scene of its name.
		ResetScene(CreateRef<Scene>(entry.SceneName), UUID::Null());
		return true;
	}

	bool EditorContext::ApplyRecovery(const RecoveryData& data, size_t* outApplied, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const RecoveryEntry& entry = data.Entry;
		if (!m_AssetManager)
			return fail("No project is open");
		if (IsPlaying())
			return fail("Stop play mode first (play.stop): recovering replaces the edited scene");
		if (!CheckSceneEditable(outError))
			return false;
		// What BeginRecovery opened or made. Opening it may have recorded a step already (the prefab catch-up): the recovered
		// entity states apply over it, as they would have over the scene its file holds.
		const bool isEntryScene = entry.Scene.IsValid() ? m_SceneHandle == entry.Scene : !m_SceneHandle.IsValid() && m_EditScene->GetName() == entry.SceneName;
		if (!isEntryScene)
			return fail(fmt::format("The edited scene is no longer '{}' (another scene replaced it meanwhile): run scene.recover again", entry.SceneName));

		Scene& scene = *m_EditScene;
		// Every entity the steps touch; for a snapshot also every entity it lacks, which goes.
		std::vector<UUID> ids;
		std::unordered_set<UUID> listed;
		for (const std::vector<EntityState>& step : data.Steps)
		{
			for (const EntityState& state : step)
			{
				if (listed.insert(state.ID).second)
					ids.push_back(state.ID);
			}
		}
		std::vector<EntityState> removals;
		if (entry.Complete)
		{
			// A world's snapshot holds the cells that were loaded then: the entities of other cells (loaded now) stay.
			std::unordered_set<AssetHandle> recordedCells;
			if (m_World)
			{
				for (const AssetHandle cell : EditorWorld::CollectCells(data.Steps))
					recordedCells.insert(cell);
			}
			for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			{
				if (m_World)
				{
					const SubSceneStatus* status = scene.GetSubSceneStatus(scene.GetSubSceneOf(entity));
					if (status && status->Scene.IsValid() && !recordedCells.contains(status->Scene))
						continue;
				}
				if (listed.insert(entity.GetUUID()).second)
				{
					ids.push_back(entity.GetUUID());
					EntityState removal;
					removal.ID = entity.GetUUID();
					removals.push_back(std::move(removal));
				}
			}
		}

		std::vector<EntityState> before = SceneEdit::CaptureEntities(scene, ids);
		const SceneSettings settingsBefore = scene.GetSettings();
		// Step by step, as redo would apply them: each step's positions are those of the scene after the step before.
		for (const std::vector<EntityState>& step : data.Steps)
		{
			if (removals.empty())
			{
				SceneEdit::ApplyEntities(scene, step);
				continue;
			}
			std::vector<EntityState> states = step;
			states.insert(states.end(), removals.begin(), removals.end());
			SceneEdit::ApplyEntities(scene, states);
		}
		SceneSettings settingsAfter = settingsBefore;
		if (!data.Settings.is_null() && !SceneRecovery::DeserializeSettings(data.Settings, settingsAfter))
			ST_WARN("Recovering '{}': its scene settings could not be read; the scene keeps its own", entry.SceneName);
		scene.GetSettings() = settingsAfter;
		std::vector<EntityState> after = SceneEdit::CaptureEntities(scene, ids);
		m_UndoStack.BreakMerge();
		m_UndoStack.Record(CreateScope<SceneRecoveryAction>(scene, std::move(before), std::move(after), settingsBefore, settingsAfter));
		PruneSelection();
		// The journal of the edited scene holds these changes from now on: once it is written, the file that held them goes.
		m_Recovery->Replace(scene, IsSceneModified(), entry.Id);
		if (outApplied)
			*outApplied = ids.size();
		ST_INFO("Recovered the unsaved changes of '{}' ({} entities); undo returns to the saved scene", entry.SceneName, ids.size());
		return true;
	}

	void EditorContext::NotifyDiscardedChanges(const std::string& sceneName, DiscardOutcome outcome, const std::string& reason, DiscardNotice notice)
	{
		// Who discarded them: the command that runs now, from its top (a command another one runs acts for that one). Without
		// one, or from the interface, the person answered the unsaved-changes question themselves.
		const CommandJournalEntry* command = m_Journal->Find(m_Journal->GetOutermostRunningId());
		const bool byPerson = !command || command->Source == CommandSource::UI || command->Source == CommandSource::Internal;
		// The person knows what they chose; when nothing could be kept there is nothing to offer them either.
		if (byPerson && outcome == DiscardOutcome::Lost)
			return;

		EditorNotification notification;
		notification.Level = byPerson ? NotificationLevel::Info : NotificationLevel::Warning;
		if (byPerson)
			notification.Title = "Unsaved changes discarded";
		else if (command->Source == CommandSource::Automation)
			notification.Title = fmt::format("{} discarded unsaved changes", command->Client.empty() ? std::string("An agent") : command->Client);
		else
			notification.Title = "A command script discarded unsaved changes";
		const std::string what = byPerson ? fmt::format("The changes to '{}'", sceneName) : fmt::format("{} replaced the scene '{}'. Its changes", command->Command, sceneName);
		if (outcome == DiscardOutcome::Lost)
		{
			notification.Message = fmt::format("{} are gone: {}.", what, reason);
		}
		else if (notice == DiscardNotice::ProjectCloses && m_Project)
		{
			notification.Message = fmt::format("{} are kept as recovery data of the project '{}', which offers them when it opens again.", what,
				m_Project->GetConfig().Name);
		}
		else
		{
			notification.Message = fmt::format("{} are kept as recovery data.", what);
			notification.ShowRecovery = true;
		}
		if (command)
			notification.JournalEntry = command->Id;
		m_Notifications.Post(std::move(notification));
	}

	void EditorContext::NotifyAssetsTrashed(const std::string& path, bool folder, size_t assets, bool referencesLeft)
	{
		// Who deleted it, as for discarded changes: the person saw their own delete happen (and answered its question).
		const CommandJournalEntry* command = m_Journal->Find(m_Journal->GetOutermostRunningId());
		if (!command || command->Source == CommandSource::UI || command->Source == CommandSource::Internal)
			return;

		EditorNotification notification;
		notification.Level = referencesLeft ? NotificationLevel::Warning : NotificationLevel::Info;
		const std::string who = command->Source == CommandSource::Automation ? (command->Client.empty() ? std::string("An agent") : command->Client)
			: std::string("A command script");
		notification.Title = fmt::format("{} deleted {}", who, folder ? "a folder" : "an asset");
		const std::string what = folder ? fmt::format("'{}' and its {} {}", path, assets, assets == 1 ? "asset" : "assets") : fmt::format("'{}'", path);
		// The step is the edited scene's: opening or creating another scene clears it. The trash keeps the files meanwhile.
		notification.Message = fmt::format("{} went to the project's trash{}; Undo restores {} while this scene stays open (the trash keeps {} {} "
			"days).", what, referencesLeft ? ", leaving references to it missing" : "", folder ? "them" : "it", folder ? "them" : "it",
			c_TrashKeepTime.count() / 24);
		notification.JournalEntry = command->Id;
		m_Notifications.Post(std::move(notification));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Scripts
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::OpenScriptEngine(bool created)
	{
		m_ScriptEngine = CreateRef<ScriptEngine>();
		ScriptEngine::SetActive(m_ScriptEngine);
		m_LastScriptBuildLoad = {};
		m_LastScriptFault.reset();
		// Before the module loads: with hot reload it runs from a private copy, so script.build can replace its file (a
		// module loaded in place locks it on Windows).
		m_ScriptEngine->SetHotReloadEnabled(m_Specification.HotReloadScripts);

		const std::filesystem::path module = m_Project->GetScriptModulePath();
		std::string error;
		if (FileSystem::IsRegularFile(module) && !LoadScriptModule(module, &error))
			ST_WARN("The project's script module '{}' could not be loaded: {}", FileSystem::ToUTF8(module), error);
		// Scripts that are not built only matter once a scene uses them: a new project's example is no reason to nag. The
		// scan says so (UpdateScriptUsageScan); a project just created has nothing to scan.
		m_ScriptUsageReported = created;
	}

	void EditorContext::StartScriptUsageScan()
	{
		if (m_ScriptUsageScan)
			m_ScriptUsageScan->Cancelled = true;
		m_ScriptUsageScan.reset();
		m_SavedScenesUseScripts = false;
		if (!m_AssetManager)
			return;

		std::vector<std::filesystem::path> files;
		for (const AssetType type : { AssetType::Scene, AssetType::Prefab })
		{
			for (const AssetMetadata& metadata : m_AssetManager->GetAllMetadata(type))
			{
				if (!metadata.IsSubAsset())
					files.push_back(m_AssetManager->GetAbsolutePath(metadata.Handle));
			}
		}
		Ref<ScriptUsageScan> scan = CreateRef<ScriptUsageScan>();
		m_ScriptUsageScan = scan;
		// Read in chunks: a scene of a million entities is hundreds of megabytes, none of which needs to be in memory at once.
		JobSystem::SubmitIO([scan, files = std::move(files)]()
		{
			for (const std::filesystem::path& file : files)
			{
				if (scan->Cancelled)
					break;
				if (DocumentAttachesScripts(file, scan->Cancelled))
				{
					scan->UsesScripts = true;
					scan->File = FileSystem::ToUTF8(file);
					break;
				}
			}
			scan->Done.store(true, std::memory_order_release);
		});
		UpdateScriptUsageScan();
	}

	void EditorContext::UpdateScriptUsageScan()
	{
		if (!m_ScriptUsageScan || !m_ScriptUsageScan->Done.load(std::memory_order_acquire))
			return;
		m_SavedScenesUseScripts = m_ScriptUsageScan->UsesScripts;
		const std::string file = m_ScriptUsageScan->File;
		m_ScriptUsageScan.reset();
		if (m_ScriptUsageReported || !m_SavedScenesUseScripts || !m_Project || !m_ScriptEngine || m_ScriptEngine->IsModuleLoaded() || !HasScriptBuild())
			return;
		m_ScriptUsageReported = true;
		if (!FileSystem::IsRegularFile(m_Project->GetScriptModulePath()))
		{
			ST_WARN("The scripts of '{}' are not built yet, and '{}' uses them; build them with script.build (Scripts > Build Scripts), or press "
				"Play, which builds them first", m_Project->GetConfig().Name, file);
		}
	}

	bool EditorContext::DoScenesUseScripts() const
	{
		if (m_SavedScenesUseScripts)
			return true;
		if (!m_EditScene)
			return false;
		// The const registry looks the storage up without creating it.
		const entt::registry& registry = m_EditScene->GetRegistry();
		const auto* scripts = registry.storage<ScriptComponent>();
		return scripts && !scripts->empty();
	}

	std::optional<std::string> EditorContext::GetPlayBuildReason() const
	{
		if (!m_Project || !m_ScriptEngine || !HasScriptBuild())
			return std::nullopt;
		const std::filesystem::path module = m_Project->GetScriptModulePath();
		std::error_code error;
		const std::filesystem::path projectModule = std::filesystem::absolute(module, error).lexically_normal();
		if (m_ScriptEngine->IsModuleLoaded())
		{
			// A module loaded from elsewhere (script.load) is the one to run.
			if (m_ScriptEngine->GetModulePath() != projectModule)
				return std::nullopt;
			const std::filesystem::path scripts = m_Project->GetScriptSourceDirectory();
			const auto describe = [&](const std::string& file, const char* what)
			{
				const std::filesystem::path relative = (scripts / FileSystem::FromUTF8(file)).lexically_relative(m_Project->GetProjectDirectory());
				return fmt::format("'{}' {} after the scripts were built", FileSystem::ToUTF8(relative), what);
			};
			const ScriptSourceListing sources = StampScriptSources(scripts);
			const std::optional<int64_t> moduleTime = FileSystem::GetLastWriteTime(module);
			const std::optional<uint64_t> moduleSize = FileSystem::GetFileSize(module);
			// The module the editor's last build left: what changed since that build compiled them. A build that left the
			// module as it was (nothing it compiles changed) settles it like one that wrote a new module.
			if (m_BuiltScriptSources && moduleTime == m_BuiltScriptSources->ModuleTime && moduleSize == m_BuiltScriptSources->ModuleSize)
			{
				if (const std::optional<ScriptSourceChange> change = FindScriptSourceChange(m_BuiltScriptSources->Sources, sources))
					return describe(change->Path, ScriptSourceChangeKindToString(change->Kind));
				return std::nullopt;
			}
			// A module built elsewhere (an IDE, an earlier session): what it was built from is unknown, so a source newer than
			// it is the sign.
			const ScriptSourceFile* newest = nullptr;
			for (const ScriptSourceFile& file : sources.Files)
			{
				if (!newest || file.LastWriteTime > newest->LastWriteTime)
					newest = &file;
			}
			if (moduleTime && newest && newest->LastWriteTime > *moduleTime)
				return describe(newest->Path, "changed");
			return std::nullopt;
		}
		if (!DoScenesUseScripts())
			return std::nullopt;
		return FileSystem::IsRegularFile(module) ? std::string("the project's script module is not loaded (see the Console)")
			: std::string("the project's scripts are not built yet");
	}

	bool EditorContext::StartPlayBuild(std::string* outError)
	{
		if (!BuildScripts(outError))
			return false;
		m_PlayBuildID = m_ScriptBuilder.GetCurrentID();
		return true;
	}

	void EditorContext::CancelScriptBuild()
	{
		if (!m_ScriptBuilder.IsRunning())
			return;
		m_ScriptBuilder.Cancel();
		OnScriptBuildFinished();
	}

	bool EditorContext::OpenScriptSource(const std::filesystem::path& file, uint32_t line, uint32_t column, std::string* outHow, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		if (!m_Project)
			return fail("No project is open (project.open or project.create)");
		const std::filesystem::path scripts = m_Project->GetScriptSourceDirectory();
		if (file.empty())
		{
			if (!FileSystem::IsDirectory(scripts))
				return fail(fmt::format("The project has no script directory '{}' yet (script.init or script.create makes it)", FileSystem::ToUTF8(scripts)));
			return m_ExternalEditor->Open(scripts, 0, 0, scripts, outHow, outError);
		}

		// Only files of the project and the engine, and only code, CMake and text: the default application of other types
		// may run them.
		std::error_code error;
		const std::filesystem::path absolute = std::filesystem::weakly_canonical(file.is_relative() ? m_Project->GetProjectDirectory() / file : file, error);
		if (error || !FileSystem::IsRegularFile(absolute))
			return fail(fmt::format("There is no file '{}'", FileSystem::ToUTF8(file)));
		const auto inside = [&absolute](const std::filesystem::path& directory)
		{
			std::error_code canonicalError;
			const std::filesystem::path root = std::filesystem::weakly_canonical(directory, canonicalError);
			if (canonicalError || root.empty())
				return false;
			const std::filesystem::path relative = absolute.lexically_relative(root);
			return !relative.empty() && *relative.begin() != "..";
		};
		if (!inside(m_Project->GetProjectDirectory()) && !inside(m_Specification.ScriptBuild.EngineDirectory))
			return fail(fmt::format("'{}' is neither in the project nor in the engine", FileSystem::ToUTF8(absolute)));
		static constexpr std::string_view c_Extensions[] = { ".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".cmake",
			".txt", ".json", ".md" };
		const std::string extension = StringUtils::ToLower(FileSystem::ToUTF8(absolute.extension()));
		if (std::find(std::begin(c_Extensions), std::end(c_Extensions), extension) == std::end(c_Extensions))
			return fail(fmt::format("'{}' is not a source file: only code, CMake and text files open in the external editor", FileSystem::ToUTF8(absolute)));
		return m_ExternalEditor->Open(absolute, line, column, scripts, outHow, outError);
	}

	void EditorContext::CloseScriptEngine()
	{
		m_ScriptBuilder.Cancel();
		m_PlayBuildID = 0;
		m_BuildingScriptSources.reset();
		m_BuiltScriptSources.reset();
		if (m_ScriptUsageScan)
			m_ScriptUsageScan->Cancelled = true;
		m_ScriptUsageScan.reset();
		m_SavedScenesUseScripts = false;
		m_ScriptUsageReported = false;
		if (!m_ScriptEngine)
			return;
		m_ScriptEngine->UnloadModule();
		if (ScriptEngine::GetActive() == m_ScriptEngine)
			ScriptEngine::SetActive(nullptr);
		m_ScriptEngine.reset();
		m_LastScriptFault.reset();
		RecordScriptModuleFile(std::nullopt);
	}

	bool EditorContext::LoadScriptModule(const std::filesystem::path& path, std::string* outError)
	{
		if (!m_ScriptEngine)
		{
			if (outError)
				*outError = "No project is open";
			return false;
		}
		const std::optional<Sha256Digest> digest = HashFile(path);
		if (!m_ScriptEngine->LoadModule(path, outError))
			return false;
		RecordScriptModuleFile(digest);
		m_LastScriptFault.reset();
		return true;
	}

	bool EditorContext::ReloadScripts(std::string* outError)
	{
		if (!m_ScriptEngine)
		{
			if (outError)
				*outError = "No project is open";
			return false;
		}
		if (m_ScriptEngine->IsModuleLoaded())
		{
			const std::optional<Sha256Digest> digest = HashFile(m_ScriptEngine->GetModulePath());
			if (!m_ScriptEngine->Reload(outError))
				return false;
			RecordScriptModuleFile(digest);
			m_LastScriptFault.reset();
			return true;
		}
		const std::filesystem::path module = m_Project->GetScriptModulePath();
		if (!FileSystem::IsRegularFile(module))
		{
			if (outError)
			{
				*outError = fmt::format("No script module is loaded and the project's scripts are not built ('{}' is missing); run script.build",
					FileSystem::ToUTF8(module));
			}
			return false;
		}
		return LoadScriptModule(module, outError);
	}

	void EditorContext::RecordScriptModuleFile(const std::optional<Sha256Digest>& expected)
	{
		m_ScriptModuleDigest.reset();
		m_ScriptModuleLoadCount = m_ScriptEngine ? m_ScriptEngine->GetLoadCount() : 0;
		if (!m_ScriptEngine || !m_ScriptEngine->IsModuleLoaded())
			return;
		const std::optional<Sha256Digest> digest = HashFile(m_ScriptEngine->GetModulePath());
		if (digest && (!expected || *expected == *digest))
			m_ScriptModuleDigest = digest;
	}

	bool EditorContext::ReadRunningScriptModule(ScriptModuleFile& outFile, std::string* outError) const
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		outFile = {};
		if (m_ScriptBuilder.IsRunning())
		{
			return fail(fmt::format("Script build {} is running and may be writing the script module; wait for it to finish (script.status) and try again",
				m_ScriptBuilder.GetCurrentID()));
		}
		if (!m_ScriptEngine || !m_ScriptEngine->IsModuleLoaded())
			return true;

		const std::filesystem::path& path = m_ScriptEngine->GetModulePath();
		std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(path);
		if (!bytes)
			return fail(fmt::format("Cannot read the script module '{}'", FileSystem::ToUTF8(path)));
		if (!m_ScriptModuleDigest || Sha256::Hash(*bytes) != *m_ScriptModuleDigest)
		{
			return fail(fmt::format("The script module '{}' changed since the editor loaded it (e.g. a build whose module could not be loaded, or a "
				"build outside the editor): load it (script.reload) or build the scripts (script.build), so that the game ships the scripts the "
				"editor runs", FileSystem::ToUTF8(path)));
		}
		outFile.Path = path;
		outFile.Bytes = std::move(*bytes);
		return true;
	}

	bool EditorContext::BuildScripts(std::string* outError)
	{
		if (!m_Project || !m_ScriptEngine)
		{
			if (outError)
				*outError = "No project is open (project.open or project.create)";
			return false;
		}
		// What this build compiles: the sources as they are now, before the compiler reads them (a file saved later counts as
		// changed after the build). The build configures its tree again when their set changed since it was configured, and
		// GetPlayBuildReason compares them with the files once the build succeeded.
		ScriptSourceListing sources = StampScriptSources(m_Project->GetScriptSourceDirectory());
		// A refused request (e.g. while a build runs) leaves the file watcher alone: a running build has paused it.
		if (!m_ScriptBuilder.Start(*m_Project, m_Specification.ScriptBuild, sources, outError))
			return false;
		m_BuildingScriptSources = std::move(sources);
		m_BuildingScriptSourcesID = m_ScriptBuilder.GetCurrentID();
		// The build reloads the module itself once it finished; the file watcher would reload it a second time when the
		// linker writes it.
		m_ScriptEngine->SetHotReloadEnabled(false);
		EditorNotification notification;
		notification.Level = NotificationLevel::Info;
		notification.Title = "Building scripts";
		notification.Message = fmt::format("{} (output in the Console)", m_Project->GetScriptModuleName());
		notification.ShowConsole = true;
		m_BuildNotification = m_Notifications.Post(std::move(notification));
		return true;
	}

	const ScriptBuildSettings& EditorContext::GetScriptBuildSettings() const
	{
		const ScriptBuildSettings* found = m_ScriptBuilder.GetFoundToolchain(m_Specification.ScriptBuild);
		return found ? *found : m_Specification.ScriptBuild;
	}

	bool EditorContext::HasScriptBuild() const
	{
		return m_Project && FileSystem::IsRegularFile(m_Project->GetScriptSourceDirectory() / "CMakeLists.txt");
	}

	bool EditorContext::IsAnnouncedLogEntry(uint64_t sequence) const
	{
		return sequence != 0 && std::find(m_AnnouncedLogEntries.begin(), m_AnnouncedLogEntries.end(), sequence) != m_AnnouncedLogEntries.end();
	}

	void EditorContext::OnScriptBuildFinished()
	{
		const ScriptBuildResult& result = m_ScriptBuilder.GetLastResult();
		// The failure's error is the build pill's and the build toast's news: not an unread error besides.
		if (result.LoggedError != 0)
		{
			m_AnnouncedLogEntries.push_back(result.LoggedError);
			// A few suffice: the Console reads new entries every frame.
			constexpr size_t c_MaxAnnouncedLogEntries = 16;
			while (m_AnnouncedLogEntries.size() > c_MaxAnnouncedLogEntries)
				m_AnnouncedLogEntries.pop_front();
		}
		m_LastScriptBuildLoad = {};
		m_LastScriptBuildLoad.BuildID = result.ID;
		// The watcher comes back before the built module loads: with hot reload the module loads from a private copy, so
		// the next build can replace the file. It only reports changes from now on, so it does not reload this build again.
		if (m_ScriptEngine)
			m_ScriptEngine->SetHotReloadEnabled(m_Specification.HotReloadScripts);
		std::optional<ScriptSourceListing> compiled = std::move(m_BuildingScriptSources);
		m_BuildingScriptSources.reset();
		if (result.Success && compiled && m_BuildingScriptSourcesID == result.ID)
		{
			const std::optional<int64_t> moduleTime = FileSystem::GetLastWriteTime(result.Module);
			const std::optional<uint64_t> moduleSize = FileSystem::GetFileSize(result.Module);
			if (moduleTime && moduleSize)
				m_BuiltScriptSources = BuiltScriptSources { std::move(*compiled), *moduleTime, *moduleSize };
		}
		if (result.Success && m_ScriptEngine)
		{
			std::error_code error;
			const std::filesystem::path built = std::filesystem::absolute(result.Module, error).lexically_normal();
			const bool loaded = m_ScriptEngine->IsModuleLoaded() && m_ScriptEngine->GetModulePath() == built;
			// An unchanged module is loaded again when it crashed: a successful build is the documented way back to a
			// working module, also when the crash came from data (fields) rather than code.
			if (loaded && !result.ModuleChanged && !m_ScriptEngine->IsFaulted())
			{
				m_LastScriptBuildLoad.Loaded = true;
			}
			else
			{
				const bool replacing = m_ScriptEngine->IsModuleLoaded();
				m_LastScriptBuildLoad.Loaded = LoadScriptModule(result.Module, &m_LastScriptBuildLoad.Error);
				m_LastScriptBuildLoad.Reloaded = replacing && m_LastScriptBuildLoad.Loaded;
			}
		}

		EditorNotification notification;
		notification.ShowConsole = true;
		if (result.Cancelled)
		{
			// Someone stopped it (the person, a command, a project closing): no failure, nothing to fix.
			notification.Level = NotificationLevel::Info;
			notification.Title = "Script build cancelled";
		}
		else if (!result.Success)
		{
			notification.Level = NotificationLevel::Error;
			notification.Title = "Script build failed";
			// The Problems panel lists the diagnostics with their places, which a double click opens.
			notification.ShowProblems = true;
			// A diagnostic without a file (no toolchain, a CMake message) is its message.
			notification.Message = result.Diagnostics.empty() ? result.Error : result.Diagnostics.front().File.empty() ? result.Diagnostics.front().Message
				: fmt::format("{}{}{}: {}", FileSystem::ToUTF8(FileSystem::FromUTF8(result.Diagnostics.front().File).filename()), result.Diagnostics.front().Line > 0 ? ":" : "",
					result.Diagnostics.front().Line > 0 ? std::to_string(result.Diagnostics.front().Line) : std::string(), result.Diagnostics.front().Message);
		}
		else if (!m_LastScriptBuildLoad.Error.empty())
		{
			notification.Level = NotificationLevel::Error;
			notification.Title = "Scripts built, but not loaded";
			notification.Message = m_LastScriptBuildLoad.Error;
		}
		else
		{
			notification.Level = NotificationLevel::Success;
			notification.Title = m_LastScriptBuildLoad.Reloaded ? "Scripts built and reloaded" : "Scripts built";
			notification.Message = fmt::format("{:.1f} s", result.Seconds);
		}
		// In the place of "Building scripts", when it still shows: one toast that changes, not two side by side.
		m_Notifications.Replace(m_BuildNotification, std::move(notification));
		m_BuildNotification = 0;
	}

	bool EditorContext::StopOnScriptFault()
	{
		if (m_SceneState != SceneState::Play || !m_ScriptEngine)
			return false;
		std::optional<ScriptFault> fault = m_ScriptEngine->GetFault();
		if (!fault)
			return false;

		const std::string where = fault->ClassName.empty() ? fmt::format("module '{}' ({})", fault->ModuleName, fault->Method)
			: fmt::format("{}::{} on entity '{}' ({})", fault->ClassName, fault->Method, fault->EntityName, fault->Entity.ToString());
		const std::string location = fault->Symbolized && fault->Location ? " at " + DescribeScriptCodeLocation(*fault->Location) : std::string();
		ST_ERROR("Script crash in {}{}: {}. Play mode stopped; fix the script and rebuild (script.build) or reload it (script.reload)", where, location,
			fault->Description);
		// Symbolization runs on a job: its location is reported when it arrives (UpdateScriptFaultLocation).
		m_ScriptFaultLocationPending = !fault->Symbolized;
		EditorNotification notification;
		notification.Level = NotificationLevel::Error;
		notification.Title = "Script crash: play mode stopped";
		notification.Message = fmt::format("{}: {}", where, fault->Description);
		notification.ShowConsole = true;
		// The Problems panel leads with the crash: where it happened and the stack, once symbolized.
		notification.ShowProblems = true;
		m_Notifications.Post(std::move(notification));
		m_LastScriptFault = std::move(fault);
		Stop();
		return true;
	}

	void EditorContext::UpdateScriptFaultLocation()
	{
		if (!m_ScriptFaultLocationPending || !m_LastScriptFault)
			return;
		const std::optional<ScriptFault> current = m_ScriptEngine ? m_ScriptEngine->GetFault() : std::nullopt;
		// The module was reloaded or unloaded meanwhile: its crash is gone, and so is the symbolization.
		if (!current || current->Method != m_LastScriptFault->Method || current->Entity != m_LastScriptFault->Entity
			|| current->Description != m_LastScriptFault->Description)
		{
			m_ScriptFaultLocationPending = false;
			return;
		}
		if (!current->Symbolized)
			return;
		m_ScriptFaultLocationPending = false;
		m_LastScriptFault->Location = current->Location;
		m_LastScriptFault->Stack = current->Stack;
		m_LastScriptFault->Symbolized = true;
		if (current->Location)
		{
			ST_ERROR("The script crash in {}::{} happened at {} (click to open it)", current->ClassName.empty() ? current->ModuleName : current->ClassName,
				current->Method, DescribeScriptCodeLocation(*current->Location));
		}
	}

	void EditorContext::CreateAnimationPreview()
	{
		// Skinned characters stand in their rest pose while the scene is edited, as they do in play before their first
		// animation; the scene's animators are found through their component's signals from here on.
		AnimationPreviewSpecification specification;
		specification.RestPoses = true;
		m_AnimationPreview = CreateScope<AnimationPreview>(*m_EditScene, specification);
	}

	AnimationPreview& EditorContext::GetAnimationPreview()
	{
		ST_CORE_ASSERT(m_AnimationPreview, "The edited scene has its animation preview from the start");
		return *m_AnimationPreview;
	}

	void EditorContext::Update(Timestep timestep)
	{
		// Script modules are only replaced here, outside scene updates.
		if (m_ScriptEngine)
		{
			m_ScriptEngine->Update();
			// A hot reload by the file watcher: the file is complete (the engine waits for that) and was just loaded.
			if (m_ScriptEngine->GetLoadCount() != m_ScriptModuleLoadCount)
				RecordScriptModuleFile(std::nullopt);
		}
		if (m_ScriptBuilder.Update())
			OnScriptBuildFinished();
		UpdateScriptUsageScan();
		UpdateScriptFaultLocation();

		if (m_AssetManager)
		{
			m_AssetManager->Update();
			if (m_AssetManager->HasImportFailures())
				NotifyImportFailures();
		}
		// Saves written in the background are published (and their completions run) between frames.
		m_DocumentSaver.Update();
		if (m_ProjectFileWriter)
			m_ProjectFileWriter->Update();
		m_Preferences->Update();
		// The asset scan or the project's migration ended: what waited for them goes on (the start scene, the script usage scan).
		UpdateProjectMigration(false);
		FinishAssetScan();
		// One budget for the frame's loading work: a scene being opened, the running game's switch and the updated scene's
		// sub-scenes draw from it.
		SceneWorkBudget budget = SceneWorkBudget::ForFrame((m_RuntimeScene ? m_RuntimeScene : m_EditScene)->GetSettings());
		// A scene being opened replaces the edited one between frames, before anything draws or updates it.
		UpdateSceneLoad(&budget);
		if (m_GameExport)
		{
			m_GameExport->Update();
			if (m_GameExport->IsFinished() && !m_GameExportNotified)
			{
				m_GameExportNotified = true;
				NotifyExportFinished(*m_GameExport);
			}
		}
		// An inspected asset that was deleted (also outside the editor) is no longer shown.
		if (m_InspectedAsset.IsValid() && !AssetManager::IsHandleValid(m_InspectedAsset))
			InspectEntities();
		if (m_RuntimeScene)
		{
			UpdateRuntimeSceneLoad(budget);
			const uint64_t stepsBefore = GetFixedStepCount();
			m_RuntimeScene->OnUpdateRuntime(timestep, &budget);
			// Time standing still (time scale 0, a pause menu) runs frames but no fixed step: taps count those frames instead.
			if (m_RuntimeScene && !m_RuntimeScene->IsPaused() && m_RuntimeScene->GetTimeScale() <= 0.0f && GetFixedStepCount() == stepsBefore)
				m_FrozenInputFrames++;
			// A crashed game stops; one that still runs gets what it asked for.
			if (!StopOnScriptFault())
				HandleRuntimeRequests();
		}
		else
		{
			// Instances of prefabs that changed take their new values; not while a scene is being opened, whose edits would be
			// rolled back (the opened scene is caught up with once it is in).
			if (!IsSceneLoading())
				m_PrefabSync.Update(*this);
			// The world's cells around the editor camera; the scene's update below advances their loads and unloads.
			if (m_World)
			{
				ST_PROFILE_STAGE("World.Streaming");
				const EditorCamera& camera = m_Viewport.GetCamera();
				m_World->Update(camera.GetPosition(), camera.GetForward(), m_Selection, m_SelectionVersion);
			}
			m_EditScene->OnUpdateEditor(timestep, &budget);
		}
		// Before the viewports render: previews whose assets arrived get their poses, animators without a request the edit pose
		// the viewport settings ask for (a change of it looks at every animator once).
		if (m_AnimationPreview)
		{
			m_AnimationPreview->SetEditPose(m_Viewport.GetSettings().EditPose);
			m_AnimationPreview->Update();
		}
		// After the game's update and before the next input frame: taps whose frames passed let go, and input frames stop
		// while the game is paused (a step was used up, or play mode stopped).
		// An input sequence counts the step the game ran and gives the next one its events (taps it made end below).
		if (m_InputSequence)
			m_InputSequence->Update(*this);
		m_SimulatedInput.Update(GetInputStepCount());
		UpdateInputSuspension();
		PruneSelection();
		m_Viewport.UpdatePicking(*this);
		UpdatePendingFrame();
		if (m_Thumbnails)
			m_Thumbnails->Update();
		m_ProjectThumbnail->Update(*this);
		FollowSceneMoves();
		// The recovery journal hands what changed to an I/O job when it is due.
		m_Recovery->Update(*m_EditScene, IsSceneModified());
	}

	void EditorContext::FollowSceneMoves()
	{
		if (!m_AssetManager)
			return;
		const uint64_t version = m_AssetManager->GetMetadataVersion();
		if (version == m_SceneMetadataVersion)
			return;
		m_SceneMetadataVersion = version;
		const auto [sceneFile, scenePath] = GetSceneFile(m_SceneHandle);
		m_Recovery->OnSceneMoved(sceneFile, scenePath);
		// A scan in progress compares the scenes' places when it finishes (SceneRecovery::FinishScan).
		for (const RecoveryEntry& entry : m_Recovery->GetEntries())
		{
			if (!entry.Scene.IsValid())
				continue;
			const std::string path = GetSceneFile(entry.Scene).second;
			if (!path.empty() && path != entry.ScenePath)
			{
				m_Recovery->RequestScan();
				break;
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Export
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::NotifyExportFinished(const GameExport& exporter)
	{
		EditorNotification notification;
		switch (exporter.GetState())
		{
			case GameExportState::Succeeded:
			{
				notification.Level = NotificationLevel::Success;
				notification.Title = "Game exported";
				const double seconds = exporter.GetResult().Seconds;
				notification.Message = fmt::format("{} ({} assets, {})", FileSystem::ToUTF8(exporter.GetOptions().Directory), exporter.GetResult().AssetCount,
					seconds < 1.0 ? fmt::format("{:.0f} ms", seconds * 1000.0) : fmt::format("{:.1f} s", seconds));
				break;
			}
			case GameExportState::Failed:
				notification.Level = NotificationLevel::Error;
				notification.Title = "Export failed";
				notification.Message = exporter.GetError();
				notification.ShowConsole = true;
				break;
			case GameExportState::Cancelled:
				notification.Level = NotificationLevel::Info;
				notification.Title = "Export cancelled";
				notification.Message = FileSystem::ToUTF8(exporter.GetOptions().Directory);
				break;
			case GameExportState::WaitingForImports:
			case GameExportState::CookingWorlds:
			case GameExportState::Packing:
			case GameExportState::Copying:
				return;
		}
		m_Notifications.Post(std::move(notification));
	}

	void EditorContext::NotifyImportFailures()
	{
		uint64_t count = 0;
		const std::vector<AssetImportFailure> failures = m_AssetManager->TakeImportFailures(&count);
		if (failures.empty())
			return;
		m_ImportFailureCount += count;
		// One toast however many fail (a project whose files are broken fails many imports as it opens): it counts them and
		// shows the latest; the Console lists them all.
		const AssetImportFailure& latest = failures.back();
		// Importers name the file in their errors ("Brick.png: unsupported image"), and the toast names its path already.
		std::string_view error = latest.Error;
		const std::string fileName = FileSystem::ToUTF8(FileSystem::FromUTF8(latest.Path).filename()) + ": ";
		if (error.starts_with(fileName))
			error.remove_prefix(fileName.size());
		EditorNotification notification;
		notification.Level = NotificationLevel::Error;
		notification.Title = "Import failed";
		notification.Message = fmt::format("{}: {}", latest.Path, error);
		notification.Asset = latest.Asset;
		notification.ShowConsole = true;
		notification.Group = "AssetImport";
		notification.Count = static_cast<uint32_t>(std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max()));
		m_Notifications.Post(std::move(notification));
	}

	bool EditorContext::StartGameExport(const GameExportOptions& options, std::string* outError)
	{
		if (m_GameExport && !m_GameExport->IsFinished())
		{
			if (outError)
				*outError = fmt::format("An export is running already (to '{}'); wait for it or cancel it (project.cancelExport)",
					FileSystem::ToUTF8(m_GameExport->GetOptions().Directory));
			return false;
		}
		Ref<GameExport> exporter = GameExport::Start(*this, options, outError);
		// An export that does not start leaves the previous one, which was announced already.
		if (!exporter)
			return false;
		m_GameExport = std::move(exporter);
		m_GameExportNotified = false;
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Editor services
	////////////////////////////////////////////////////////////////////////////////

	std::filesystem::path EditorContext::GetSamplesDirectory() const
	{
		return m_Specification.SamplesDirectory.empty() ? ProjectSamples::GetDefaultDirectory() : m_Specification.SamplesDirectory;
	}

	void EditorContext::SetStatusProvider(const std::string& section, StatusProvider provider, StatusProvider compactProvider)
	{
		if (provider)
			m_StatusProviders.insert_or_assign(section, StatusSection { std::move(provider), std::move(compactProvider) });
		else
			m_StatusProviders.erase(section);
	}

	const Ref<WorldCookCache>& EditorContext::GetWorldCookCache()
	{
		// The cells another project's cooks read mean nothing for this one (also when it is the same project opened again: its
		// files may have changed meanwhile).
		if (!m_WorldCookCache || m_WorldCookCacheGeneration != m_ProjectGeneration)
		{
			m_WorldCookCache = CreateRef<WorldCookCache>();
			m_WorldCookCacheGeneration = m_ProjectGeneration;
		}
		return m_WorldCookCache;
	}

}
