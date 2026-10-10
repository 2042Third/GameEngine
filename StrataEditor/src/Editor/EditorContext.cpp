#include "Editor/EditorContext.h"

#include "Editor/ProjectSamples.h"
#include "Editor/ScriptProject.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Audio/AudioEngine.h>
#include <Strata/Core/Crypto.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Input/Input.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/SceneSerializer.h>

#include <algorithm>

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

	}

	EditorContext::EditorContext(const EditorContextSpecification& specification)
		: m_Specification(specification), m_EditScene(CreateRef<Scene>()),
		  m_RecentProjects(specification.RecentProjectsFile, specification.RecentProjectsReadOnly)
	{
		ActivateBuiltinAssets();
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
		if (!CreateScriptProjectFiles(*project, true, nullptr, outError))
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

	bool EditorContext::OpenProject(const std::filesystem::path& path, std::string* outError)
	{
		return OpenProjectInternal(path, false, outError);
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
		Project::SetActive(m_Project);

		// The editor camera and viewport settings continue where they were when the project was last closed.
		const std::filesystem::path viewportState = GetViewportStateFile();
		std::string viewportError;
		if (FileSystem::Exists(viewportState) && !m_Viewport.Load(viewportState, &viewportError))
			ST_WARN("The saved viewport state is ignored: {}", viewportError);

		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = m_Project->GetAssetDirectory();
		specification.CacheDirectory = m_Project->GetCacheDirectory();
		specification.WatchFiles = m_Specification.WatchAssetFiles;
		m_AssetManager = CreateRef<EditorAssetManager>(specification);
		AssetManager::SetActive(m_AssetManager);
		m_AssetManager->Scan();
		ST_INFO("Opened project '{}' ({})", m_Project->GetConfig().Name, FileSystem::ToUTF8(m_Project->GetProjectDirectory()));
		OpenScriptEngine(created);

		// Continue with the start scene when the project has one.
		const AssetHandle startScene = m_Project->GetConfig().StartScene;
		std::string sceneError;
		if (startScene.IsValid() && !OpenScene(startScene, &sceneError))
			ST_WARN("Could not open the start scene: {}", sceneError);
		m_RecentProjects.Add(m_Project->GetConfig().Name, m_Project->GetProjectFile());
		return true;
	}

	void EditorContext::CloseProject()
	{
		ReleaseProject(true);
	}

	void EditorContext::ReleaseProject(bool activateBuiltinAssets)
	{
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
		ResetScene(CreateRef<Scene>(), UUID::Null());
		// After the scene went: the next project starts from the default view, with none of this project's scene cameras.
		if (hadProject)
			m_Viewport.ResetState();
		CloseScriptEngine();
		if (m_AssetManager)
		{
			if (AssetManager::GetActive() == m_AssetManager)
				AssetManager::SetActive(nullptr);
			m_AssetManager.reset();
		}
		if (m_Project)
		{
			if (Project::GetActive() == m_Project)
				Project::SetActive(nullptr);
			m_Project.reset();
		}
		if (activateBuiltinAssets)
			ActivateBuiltinAssets();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Scene
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::ResetScene(Ref<Scene> scene, AssetHandle handle)
	{
		Stop();
		// The scene that goes keeps its view for when it opens again.
		m_Viewport.StoreSceneCamera(m_SceneHandle);
		m_PendingFrame.reset();
		m_EditScene = std::move(scene);
		m_SceneHandle = handle;
		m_Selection.clear();
		m_UndoStack.Clear();
	}

	bool EditorContext::NewScene(const std::string& name, std::string_view templateId)
	{
		if (!ProjectTemplates::Find(templateId))
			return false;
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

	bool EditorContext::OpenScene(AssetHandle handle, std::string* outError)
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

		// Read the current file, not a copy loaded earlier.
		m_AssetManager->UnloadAsset(handle);
		Ref<SceneAsset> asset = AssetManager::LoadAssetSync<SceneAsset>(handle);
		if (!asset)
			return fail(fmt::format("Loading the scene failed: {}", m_AssetManager->GetAssetError(handle)));
		std::string error;
		Ref<Scene> scene = asset->CreateScene(&error);
		if (!scene)
			return fail(fmt::format("The scene is invalid: {}", error));

		ResetScene(scene, handle);
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
		return true;
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
		const std::string document = JsonUtils::Dump(SceneSerializer::Serialize(*m_EditScene), 1, '\t') + "\n";
		const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.data()), document.size());
		if (!m_AssetManager->SaveNativeAsset(m_SceneHandle, bytes, false, outError))
			return false;
		m_UndoStack.MarkSaved();
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
		const std::string document = JsonUtils::Dump(SceneSerializer::Serialize(*m_EditScene), 1, '\t') + "\n";
		const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(document.data()), document.size());
		const AssetHandle handle = m_AssetManager->CreateNativeAsset(relativePath, bytes, outError);
		if (!handle.IsValid())
		{
			m_EditScene->SetName(previousName);
			return false;
		}
		m_SceneHandle = handle;
		m_UndoStack.MarkSaved();
		return true;
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
		m_UndoStack.BreakMerge();
		m_MasterVolumeBeforePlay = AudioEngine::GetMasterVolume();
		m_RuntimeScene = Scene::Copy(m_EditScene);
		m_RuntimeSceneAsset = UUID::Null();
		// Keys a tool held down in an earlier session (input.* commands) must not leak into this one.
		ResetSimulatedInput();
		m_RuntimeScene->OnRuntimeStart(mode);
		m_SceneState = mode == SceneRuntimeMode::Play ? SceneState::Play : SceneState::Simulate;
		UpdateInputSuspension();
		return true;
	}

	void EditorContext::Stop()
	{
		if (!m_RuntimeScene)
			return;
		m_RuntimeScene->OnRuntimeStop();
		m_RuntimeScene.reset();
		m_RuntimeSceneAsset = UUID::Null();
		m_SceneState = SceneState::Edit;
		// A game's volume setting belongs to the game session, not to the editor.
		AudioEngine::SetMasterVolume(m_MasterVolumeBeforePlay);
		m_GameInputActive = false;
		ResetSimulatedInput(); // The game that a tool's simulated input was meant for is gone
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
		// included, as play mode started with them).
		const AssetHandle target = scene.IsValid() ? scene : m_RuntimeSceneAsset;
		Ref<Scene> next;
		if (!target.IsValid())
		{
			next = Scene::Copy(m_EditScene);
		}
		else
		{
			if (!m_AssetManager || m_AssetManager->GetAssetType(target) != AssetType::Scene)
				return fail(fmt::format("{} is not a scene asset of the project", target.ToString()));
			Ref<SceneAsset> asset = AssetManager::LoadAssetSync<SceneAsset>(target);
			if (!asset)
				return fail(fmt::format("loading scene {} failed: {}", target.ToString(), m_AssetManager->GetAssetError(target)));
			std::string error;
			next = asset->CreateScene(&error);
			if (!next)
				return fail(fmt::format("scene {} is invalid: {}", target.ToString(), error));
		}

		// The new scene continues the way play mode was going: paused (with the steps still to run) or running.
		const SceneRuntimeMode mode = m_RuntimeScene->GetRuntimeMode();
		const bool paused = m_RuntimeScene->IsPaused();
		const uint32_t steps = m_RuntimeScene->GetStepFrames();
		m_RuntimeScene->OnRuntimeStop();
		m_RuntimeScene = std::move(next);
		m_RuntimeSceneAsset = target;
		m_RuntimeScene->OnRuntimeStart(mode);
		if (paused)
		{
			m_RuntimeScene->SetPaused(true);
			m_RuntimeScene->Step(steps);
		}
		PruneSelection(); // The selection named entities of the previous scene
		UpdateInputSuspension();
		return true;
	}

	void EditorContext::SetGameInputActive(bool active)
	{
		m_GameInputActive = active && m_SceneState == SceneState::Play;
	}

	void EditorContext::SetPaused(bool paused)
	{
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

	////////////////////////////////////////////////////////////////////////////////
	// Selection
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::SetSelection(std::vector<UUID> selection)
	{
		m_Selection.clear();
		for (UUID entity : selection)
		{
			if (GetActiveScene()->GetEntityByUUID(entity) && !IsSelected(entity))
				m_Selection.push_back(entity);
		}
	}

	void EditorContext::Select(UUID entity, bool additive)
	{
		if (!additive)
			m_Selection.clear();
		if (!GetActiveScene()->GetEntityByUUID(entity))
			return;
		// The latest selection is the primary one: move it to the back.
		std::erase(m_Selection, entity);
		m_Selection.push_back(entity);
	}

	void EditorContext::Deselect(UUID entity)
	{
		std::erase(m_Selection, entity);
	}

	bool EditorContext::IsSelected(UUID entity) const
	{
		return std::find(m_Selection.begin(), m_Selection.end(), entity) != m_Selection.end();
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
		std::erase_if(m_Selection, [this](UUID entity) { return !GetActiveScene()->GetEntityByUUID(entity); });
	}

	////////////////////////////////////////////////////////////////////////////////
	// Undo
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::CommitEdit(SceneEditTransaction& transaction)
	{
		if (IsPlaying())
			return false;
		return transaction.Commit(m_UndoStack);
	}

	bool EditorContext::Undo()
	{
		if (IsPlaying())
			return false;
		const bool undone = m_UndoStack.Undo();
		PruneSelection();
		return undone;
	}

	bool EditorContext::Redo()
	{
		if (IsPlaying())
			return false;
		const bool redone = m_UndoStack.Redo();
		PruneSelection();
		return redone;
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
		if (FileSystem::IsRegularFile(module))
		{
			if (!LoadScriptModule(module, &error))
				ST_WARN("The project's script module '{}' could not be loaded: {}", FileSystem::ToUTF8(module), error);
		}
		else if (!created && HasScriptBuild())
		{
			ST_WARN("The scripts of '{}' are not built yet; build them with script.build (Scripts > Build Scripts)", m_Project->GetConfig().Name);
		}
	}

	void EditorContext::CloseScriptEngine()
	{
		m_ScriptBuilder.Cancel();
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
		// A refused request (e.g. while a build runs) leaves the file watcher alone: a running build has paused it.
		if (!m_ScriptBuilder.Start(*m_Project, m_Specification.ScriptBuild, outError))
			return false;
		// The build reloads the module itself once it finished; the file watcher would reload it a second time when the
		// linker writes it.
		m_ScriptEngine->SetHotReloadEnabled(false);
		return true;
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
		ST_ERROR("Script crash in {}: {}. Play mode stopped; fix the script and rebuild (script.build) or reload it (script.reload)", where,
			fault->Description);
		m_LastScriptFault = std::move(fault);
		Stop();
		return true;
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

		if (m_AssetManager)
			m_AssetManager->Update();
		if (m_RuntimeScene)
		{
			m_RuntimeScene->OnUpdateRuntime(timestep);
			// A crashed game stops; one that still runs gets what it asked for.
			if (!StopOnScriptFault())
				HandleRuntimeRequests();
		}
		else
		{
			m_EditScene->OnUpdateEditor(timestep);
		}
		// After the game's update and before the next input frame: taps whose frames passed let go, and input frames stop
		// while the game is paused (a step was used up, or play mode stopped).
		m_SimulatedInput.Update();
		UpdateInputSuspension();
		PruneSelection();
		m_Viewport.UpdatePicking(*this);
		UpdatePendingFrame();
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
