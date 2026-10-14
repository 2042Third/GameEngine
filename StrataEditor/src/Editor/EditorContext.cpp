#include "Editor/EditorContext.h"

#include "Editor/CommandUtils.h"
#include "Editor/ProjectSamples.h"
#include "Editor/ScriptProject.h"

#include <Strata/Asset/AssetDependencies.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/FileReader.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Profiling.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Input/Input.h>
#include <Strata/Renderer/RenderScene.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/SceneLoader.h>
#include <Strata/Scene/SceneSerializer.h>

#include <algorithm>
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
		: m_Specification(specification), m_EditScene(CreateRef<Scene>()), m_ExternalEditor(specification.EditorSettingsFile),
		  m_RecentProjects(specification.RecentProjectsFile, specification.RecentProjectsReadOnly)
	{
		ActivateBuiltinAssets();
		if (specification.ToolchainEnvironment)
			m_ScriptBuilder.SetToolchainEnvironment(specification.ToolchainEnvironment);
		m_GameWindowProvider = CreateScope<EngineServices::ScopedProvider<IGameWindow>>(m_GameWindow);
		m_GameSavesProvider = CreateScope<EngineServices::ScopedProvider<IGameSaves>>(m_GameSaves);
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
		m_UndoStack.SetObserver(&m_Recovery);
		m_Recovery.BeginScene(*m_EditScene, UUID::Null(), {}, {});
		CreateAnimationPreview();
		m_Recovery.SetEntryValidator([this](const RecoveryEntry& entry) -> std::string
		{
			if (!entry.Scene.IsValid())
				return {};
			if (!m_AssetManager || m_AssetManager->GetAssetType(entry.Scene) != AssetType::Scene)
				return fmt::format("The scene {} ('{}') is no longer in the project", entry.Scene.ToString(), entry.ScenePath);
			return {};
		});
		m_Recovery.SetSceneLocator([this]()
		{
			SceneRecovery::ScenePaths scenes;
			if (m_AssetManager)
			{
				for (const AssetMetadata& metadata : m_AssetManager->GetAllMetadata(AssetType::Scene))
					scenes.emplace(metadata.Handle, metadata.Path);
			}
			return scenes;
		});
		SetStatusProvider("recovery", [this]() { return m_Recovery.DescribeStatus(); });
		// The asset manager of the open project, or the built-in assets' without one (asset.stats has the details).
		SetStatusProvider("assets", [this]()
		{
			const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
			if (!manager)
				return nlohmann::json(nullptr);
			nlohmann::json description = CommandUtils::DescribeAssetStats(manager->GetStats());
			// Imports still running (a project opened with assets to import, re-imports): their assets arrive when they finish.
			description["importing"] = m_AssetManager && manager == m_AssetManager ? m_AssetManager->GetPendingImportCount() : 0u;
			return description;
		});
	}

	EditorContext::~EditorContext()
	{
		ReleaseProject(false);
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
		Project& project = *m_Project;
		project.GetConfig().StartScene = m_SceneHandle;
		if (!project.Save(&error))
		{
			if (outError)
				*outError = fmt::format("The project was created, but its start scene could not be set: {}", error);
			return false;
		}
		return true;
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
		Ref<Project> project = Project::Load(projectFile, outError);
		if (!project)
			return false;

		CloseProject();
		m_Project = project;
		m_ProjectGeneration++;
		Project::SetActive(m_Project);

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
		m_AssetManager->Scan(m_Specification.ImportInBackground ? ScanImports::InBackground : ScanImports::Wait);
		m_AssetManager->PurgeTrash(c_TrashKeepTime);
		OpenThumbnails();
		ST_INFO("Opened project '{}' ({})", m_Project->GetConfig().Name, FileSystem::ToUTF8(m_Project->GetProjectDirectory()));
		// Unsaved changes an editor left behind (a crash) or discarded are offered once the scan has read them.
		m_Recovery.SetProject(m_Project->GetIntermediateDirectory() / SceneRecovery::c_DirectoryName, m_Project->GetAssetDirectory());
		m_Recovery.Scan();
		OpenScriptEngine(created);
		StartScriptUsageScan();

		// Continue with the start scene when the project has one: it loads in the background (UpdateSceneLoad warns when it
		// cannot be opened).
		const AssetHandle startScene = m_Project->GetConfig().StartScene;
		std::string sceneError;
		if (startScene.IsValid())
		{
			if (!BeginOpenScene(startScene, &sceneError))
			{
				ST_WARN("Could not open the start scene: {}", sceneError);
				// Reported like a load that failed (project.open, scene.loadStatus).
				m_SceneLoadStatus = EditorSceneLoadStatus();
				m_SceneLoadStatus.ID = m_NextSceneLoadID++;
				m_SceneLoadStatus.Scene = startScene;
				m_SceneLoadStatus.State = SceneLoadState::Failed;
				m_SceneLoadStatus.Error = std::move(sceneError);
				m_SceneLoadStatus.Started = std::chrono::steady_clock::now();
			}
			m_SceneLoadStatus.StartScene = true;
		}
		m_RecentProjects.Add(m_Project->GetConfig().Name, m_Project->GetProjectFile());
		return true;
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
		// The export reads the project's assets on an I/O thread: it stops before they go.
		if (m_GameExport && !m_GameExport->IsFinished())
		{
			m_GameExport->Cancel();
			m_GameExport->Wait();
		}
		CancelSceneLoad();
		Stop();
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
			m_Recovery.SetProject({}, {});
		InspectEntities(); // The project's assets go with it
		// After the scene went: the next project starts from the default view, with none of this project's scene cameras.
		if (hadProject)
			m_Viewport.ResetState();
		CloseScriptEngine();
		// Before the asset manager: its reads use the manager.
		m_Thumbnails.reset();
		m_ProjectThumbnail.Cancel();
		if (m_AssetManager)
		{
			// Cancels the project's imports without waiting for them (also while something else still holds the manager).
			m_AssetManager->Close();
			if (AssetManager::GetActive() == m_AssetManager)
				AssetManager::SetActive(nullptr);
			m_AssetManager.reset();
			m_SceneMetadataVersion = UINT64_MAX;
		}
		if (m_Project)
		{
			if (Project::GetActive() == m_Project)
				Project::SetActive(nullptr);
			m_Project.reset();
			m_ProjectGeneration++;
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
		const DiscardOutcome discarded = m_Recovery.EndScene(*m_EditScene, IsSceneModified(), &lostReason);
		if (discarded != DiscardOutcome::Unmodified && notice != DiscardNotice::None)
			NotifyDiscardedChanges(m_EditScene->GetName(), discarded, lostReason, notice);
		// The scene that goes keeps its view for when it opens again.
		m_Viewport.StoreSceneCamera(m_SceneHandle);
		m_PendingFrame.reset();
		m_AnimationPreview.reset(); // Of the scene that goes, which it refers to: before the scene is released
		Ref<Scene> previous = std::exchange(m_EditScene, std::move(scene));
		CreateAnimationPreview();
		m_SceneHandle = handle;
		ClearSelection();
		m_UndoStack.Clear();
		// The scene that goes is destroyed off the frame (a million entities take tens of milliseconds).
		Scene::ReleaseInBackground(std::move(previous));
		const auto [sceneFile, scenePath] = GetSceneFile(handle);
		m_Recovery.BeginScene(*m_EditScene, handle, sceneFile, scenePath);
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

	std::function<bool(Scene&)> EditorContext::GetScenePreparation()
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
		if (!m_SceneLoad)
			return;
		m_SceneLoad->Request.Cancel();
		m_SceneLoad.reset();
		m_SceneLoadStatus.State = SceneLoadState::Cancelled;
		m_SceneLoadStatus.Error = "Opening the scene was cancelled";
		m_SceneLoadStatus.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_SceneLoadStatus.Started).count();
	}

	void EditorContext::UpdateSceneLoad()
	{
		if (!m_SceneLoad)
			return;
		ST_PROFILE_STAGE("Scene.Load");
		SceneLoadRequest& request = m_SceneLoad->Request;
		request.Update();
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

	bool EditorContext::SaveScene(std::string* outError)
	{
		if (!m_AssetManager || !m_SceneHandle.IsValid())
		{
			if (outError)
				*outError = "The scene has not been saved yet; save it under a path first";
			return false;
		}
		// What the scene references comes from the JSON at hand: saving a large scene parses no document again.
		const nlohmann::json json = SceneSerializer::Serialize(*m_EditScene);
		std::vector<AssetHandle> references;
		const bool scanned = AssetDependencies::ScanDocument(AssetType::Scene, json, references);
		std::string document = JsonUtils::Dump(json, 1, '\t') + "\n";
		const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.data()), document.size());
		if (!m_AssetManager->SaveNativeAsset(m_SceneHandle, bytes, false, outError, scanned ? &references : nullptr))
			return false;
		m_UndoStack.MarkSaved();
		OnSceneSaved();
		// The changes are on disk now: the recovery journal starts again from this document.
		const auto [sceneFile, scenePath] = GetSceneFile(m_SceneHandle);
		m_Recovery.OnSaved(m_SceneHandle, sceneFile, scenePath, std::move(document));
		return true;
	}

	bool EditorContext::SaveSceneAs(const std::string& relativePath, std::string* outError)
	{
		if (!m_AssetManager)
		{
			if (outError)
				*outError = "No project is open";
			return false;
		}
		// The scene takes the name of its file.
		const std::string previousName = m_EditScene->GetName();
		m_EditScene->SetName(FileSystem::ToUTF8(FileSystem::FromUTF8(relativePath).stem()));
		const nlohmann::json json = SceneSerializer::Serialize(*m_EditScene);
		std::vector<AssetHandle> references;
		const bool scanned = AssetDependencies::ScanDocument(AssetType::Scene, json, references);
		std::string document = JsonUtils::Dump(json, 1, '\t') + "\n";
		const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.data()), document.size());
		const AssetHandle handle = m_AssetManager->CreateNativeAsset(relativePath, bytes, outError, scanned ? &references : nullptr);
		if (!handle.IsValid())
		{
			m_EditScene->SetName(previousName);
			return false;
		}
		m_SceneHandle = handle;
		m_EditScene->SetSourceAsset(handle);
		m_UndoStack.MarkSaved();
		OnSceneSaved();
		const auto [sceneFile, scenePath] = GetSceneFile(handle);
		m_Recovery.OnSaved(handle, sceneFile, scenePath, std::move(document));
		return true;
	}

	void EditorContext::OnSceneSaved()
	{
		m_ProjectThumbnail.Request();
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
		m_RuntimeSceneAsset = UUID::Null();
		m_PlaySession++;
		m_ReplacedFixedSteps = 0;
		// Keys a tool held down in an earlier session (input.* commands) must not leak into this one.
		ResetSimulatedInput();
		ResetGameShell();
		m_RuntimeScene->SetTimeDilation(m_Specification.TimeDilation);
		m_RuntimeScene->OnRuntimeStart(mode);
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
		next->Request = SceneLoader::LoadAsync(target, options);
		m_RuntimeSceneLoad = std::move(next);
		m_RuntimeScene->SetSceneLoadProgress(0.0f);
		return true;
	}

	void EditorContext::UpdateRuntimeSceneLoad()
	{
		if (!m_RuntimeSceneLoad || !m_RuntimeScene)
			return;
		ST_PROFILE_STAGE("Scene.Load");
		SceneLoadRequest& request = m_RuntimeSceneLoad->Request;
		request.Update();
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
		// The scene that goes stops here and is destroyed off the frame (a million entities take tens of milliseconds).
		Scene::ReleaseInBackground(std::move(m_RuntimeScene));
		m_RuntimeScene = std::move(next);
		m_RuntimeSceneAsset = asset;
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
			m_FixedStepsAtResume = GetFixedStepCount();
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
			InspectEntities();
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
		InspectEntities();
	}

	void EditorContext::Deselect(UUID entity)
	{
		if (m_SelectionSet.erase(entity) > 0)
			std::erase(m_Selection, entity);
	}

	void EditorContext::ClearSelection()
	{
		m_Selection.clear();
		m_SelectionSet.clear();
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
		std::erase_if(m_Selection, [this](UUID entity)
		{
			if (GetActiveScene()->GetEntityByUUID(entity))
				return false;
			m_SelectionSet.erase(entity);
			return true;
		});
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

	bool EditorContext::CommitEdit(SceneEditTransaction& transaction)
	{
		if (IsPlaying())
			return false;
		if (IsSceneLoading())
		{
			// The edited scene is about to be replaced: an edit now would be lost with it, so it is undone at once.
			transaction.Rollback();
			return false;
		}
		return transaction.Commit(m_UndoStack);
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
			for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			{
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
		m_Recovery.Replace(scene, IsSceneModified(), entry.Id);
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
			return m_ExternalEditor.Open(scripts, 0, 0, scripts, outHow, outError);
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
		return m_ExternalEditor.Open(absolute, line, column, scripts, outHow, outError);
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
		m_Notifications.Post(std::move(notification));
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

	void EditorContext::OnScriptBuildFinished()
	{
		const ScriptBuildResult& result = m_ScriptBuilder.GetLastResult();
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
		m_Notifications.Post(std::move(notification));
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
		// A scene being opened replaces the edited one between frames, before anything draws or updates it.
		UpdateSceneLoad();
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
			UpdateRuntimeSceneLoad();
			m_RuntimeScene->OnUpdateRuntime(timestep);
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
			m_EditScene->OnUpdateEditor(timestep);
		}
		// Before the viewports render: previews whose assets arrived get their poses.
		if (m_AnimationPreview)
			m_AnimationPreview->Update();
		// After the game's update and before the next input frame: taps whose frames passed let go, and input frames stop
		// while the game is paused (a step was used up, or play mode stopped).
		m_SimulatedInput.Update();
		UpdateInputSuspension();
		PruneSelection();
		m_Viewport.UpdatePicking(*this);
		UpdatePendingFrame();
		if (m_Thumbnails)
			m_Thumbnails->Update();
		m_ProjectThumbnail.Update(*this);
		FollowSceneMoves();
		// The recovery journal hands what changed to an I/O job when it is due.
		m_Recovery.Update(*m_EditScene, IsSceneModified());
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
		m_Recovery.OnSceneMoved(sceneFile, scenePath);
		// A scan in progress compares the scenes' places when it finishes (SceneRecovery::FinishScan).
		for (const RecoveryEntry& entry : m_Recovery.GetEntries())
		{
			if (!entry.Scene.IsValid())
				continue;
			const std::string path = GetSceneFile(entry.Scene).second;
			if (!path.empty() && path != entry.ScenePath)
			{
				m_Recovery.RequestScan();
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

	void EditorContext::SetStatusProvider(const std::string& section, StatusProvider provider)
	{
		if (provider)
			m_StatusProviders.insert_or_assign(section, std::move(provider));
		else
			m_StatusProviders.erase(section);
	}

}
