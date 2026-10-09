#include "Editor/EditorContext.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
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

	EditorContext::EditorContext(const EditorContextSpecification& specification)
		: m_Specification(specification), m_EditScene(CreateRef<Scene>())
	{
	}

	EditorContext::~EditorContext()
	{
		CloseProject();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Project
	////////////////////////////////////////////////////////////////////////////////

	bool EditorContext::CreateProject(const std::filesystem::path& directory, const std::string& name, std::string* outError)
	{
		Ref<Project> project = Project::Create(directory, name, outError);
		if (!project)
			return false;
		return OpenProject(project->GetProjectFile(), outError);
	}

	bool EditorContext::OpenProject(const std::filesystem::path& path, std::string* outError)
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

		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = m_Project->GetAssetDirectory();
		specification.CacheDirectory = m_Project->GetCacheDirectory();
		specification.WatchFiles = m_Specification.WatchAssetFiles;
		m_AssetManager = CreateRef<EditorAssetManager>(specification);
		AssetManager::SetActive(m_AssetManager);
		m_AssetManager->Scan();
		ST_INFO("Opened project '{}' ({})", m_Project->GetConfig().Name, FileSystem::ToUTF8(m_Project->GetProjectDirectory()));

		// Continue with the start scene when the project has one.
		const AssetHandle startScene = m_Project->GetConfig().StartScene;
		std::string sceneError;
		if (startScene.IsValid() && !OpenScene(startScene, &sceneError))
			ST_WARN("Could not open the start scene: {}", sceneError);
		return true;
	}

	void EditorContext::CloseProject()
	{
		Stop();
		ResetScene(CreateRef<Scene>(), UUID::Null());
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
	}

	////////////////////////////////////////////////////////////////////////////////
	// Scene
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::ResetScene(Ref<Scene> scene, AssetHandle handle)
	{
		Stop();
		m_EditScene = std::move(scene);
		m_SceneHandle = handle;
		m_Selection.clear();
		m_UndoStack.Clear();
	}

	void EditorContext::NewScene(const std::string& name)
	{
		ResetScene(CreateRef<Scene>(name), UUID::Null());
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
		return true;
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
		return StartRuntime(SceneRuntimeMode::Play, outError);
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
		m_RuntimeScene = Scene::Copy(m_EditScene);
		m_RuntimeScene->OnRuntimeStart(mode);
		m_SceneState = mode == SceneRuntimeMode::Play ? SceneState::Play : SceneState::Simulate;
		return true;
	}

	void EditorContext::Stop()
	{
		if (!m_RuntimeScene)
			return;
		m_RuntimeScene->OnRuntimeStop();
		m_RuntimeScene.reset();
		m_SceneState = SceneState::Edit;
		PruneSelection(); // Entities created during play are gone
	}

	void EditorContext::SetPaused(bool paused)
	{
		if (m_RuntimeScene)
			m_RuntimeScene->SetPaused(paused);
	}

	bool EditorContext::IsPaused() const
	{
		return m_RuntimeScene && m_RuntimeScene->IsPaused();
	}

	void EditorContext::Step(uint32_t frames)
	{
		if (m_RuntimeScene && m_RuntimeScene->IsPaused())
			m_RuntimeScene->Step(frames);
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

	void EditorContext::Update(Timestep timestep)
	{
		if (m_AssetManager)
			m_AssetManager->Update();
		if (m_RuntimeScene)
			m_RuntimeScene->OnUpdateRuntime(timestep);
		else
			m_EditScene->OnUpdateEditor(timestep);
		PruneSelection();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Editor services
	////////////////////////////////////////////////////////////////////////////////

	void EditorContext::SetStatusProvider(const std::string& section, StatusProvider provider)
	{
		if (provider)
			m_StatusProviders.insert_or_assign(section, std::move(provider));
		else
			m_StatusProviders.erase(section);
	}

}
