#include "Editor/AssetFileAction.h"

#include "Editor/AssetReferences.h"
#include "Editor/EditorContext.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <algorithm>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		// The source asset at a path of the asset directory (null when none is there).
		AssetHandle FindSourceAt(const EditorAssetManager& assets, const std::string& path)
		{
			return assets.FindAssetByAbsolutePath(assets.GetAssetDirectory() / FileSystem::FromUTF8(path));
		}

	}

	AssetFileAction::AssetFileAction(EditorAssetManager& assets, std::string name)
		: m_Assets(assets.weak_from_this()), m_Name(std::move(name))
	{
	}

	EditorAssetManager* AssetFileAction::GetAssets(std::string* outError) const
	{
		const Ref<AssetManagerBase> manager = m_Assets.lock();
		if (!manager)
		{
			if (outError)
				*outError = "the project this step belongs to is closed";
			return nullptr;
		}
		// Made from an EditorAssetManager (the constructor's argument).
		return static_cast<EditorAssetManager*>(manager.get());
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetTrashAction
	////////////////////////////////////////////////////////////////////////////////

	AssetTrashAction::AssetTrashAction(EditorContext& context, EditorAssetManager& assets, std::string name, TrashedAssets trashed, bool force,
		bool openSceneFromMemory)
		: AssetFileAction(assets, std::move(name)), m_Context(context), m_Trashed(std::move(trashed)), m_Force(force), m_OpenSceneFromMemory(openSceneFromMemory)
	{
	}

	bool AssetTrashAction::CheckRedo(EditorAssetManager& assets, std::string* outError)
	{
		// The path holds what was deleted there, and only that: whatever came there since is no part of this step.
		if (!m_Trashed.Folder)
		{
			if (m_Trashed.Handles.empty() || FindSourceAt(assets, m_Trashed.Path) != m_Trashed.Handles.front())
				return Fail(outError, fmt::format("'{}' is no longer the asset this step deleted", m_Trashed.Path));
		}
		else
		{
			const std::vector<AssetHandle> below = assets.GetAssetsBelowFolder(m_Trashed.Path);
			const std::unordered_set<AssetHandle> now(below.begin(), below.end());
			const std::unordered_set<AssetHandle> deleted(m_Trashed.Handles.begin(), m_Trashed.Handles.end());
			if (!assets.HasFolder(m_Trashed.Path) || now != deleted)
				return Fail(outError, fmt::format("The folder '{}' no longer holds what this step deleted", m_Trashed.Path));
		}
		// What asset.delete and asset.deleteFolder refuse, they refuse here too.
		const AssetHandle scene = m_Context.GetSceneHandle();
		if (scene.IsValid() && std::find(m_Trashed.Handles.begin(), m_Trashed.Handles.end(), scene) != m_Trashed.Handles.end())
			return Fail(outError, fmt::format("'{}' holds the open scene, which cannot be deleted; open another scene first", m_Trashed.Path));
		if (!m_Force)
		{
			const std::vector<AssetHandle> targets = AssetReferences::WithSubAssets(m_Context, m_Trashed.Handles);
			std::vector<AssetReferencer> referencers = AssetReferences::Find(m_Context, targets, { targets.begin(), targets.end() });
			if (m_OpenSceneFromMemory)
				AssetReferences::IgnoreSavedOpenScene(referencers);
			if (!referencers.empty())
				return Fail(outError, fmt::format("'{}' is used by {} now", m_Trashed.Path, AssetReferences::Describe(referencers)));
		}
		return true;
	}

	bool AssetTrashAction::Execute(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		if (!assets || !CheckRedo(*assets, outError))
			return false;
		// Each redo makes a new trash folder.
		return assets->MoveToTrash(m_Trashed.Path, m_Trashed, outError);
	}

	bool AssetTrashAction::Undo(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		return assets && assets->RestoreFromTrash(m_Trashed, outError);
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetMoveAction
	////////////////////////////////////////////////////////////////////////////////

	AssetMoveAction::AssetMoveAction(EditorAssetManager& assets, std::string name, AssetHandle asset, std::string from, std::string to)
		: AssetFileAction(assets, std::move(name)), m_Asset(asset), m_From(std::move(from)), m_To(std::move(to))
	{
	}

	bool AssetMoveAction::Move(const std::string& to, std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		if (!assets)
			return false;
		if (m_Asset.IsValid())
			return assets->MoveAsset(m_Asset, to, outError);
		return assets->MoveFolder(to == m_To ? m_From : m_To, to, outError);
	}

	bool AssetMoveAction::Execute(std::string* outError)
	{
		return Move(m_To, outError);
	}

	bool AssetMoveAction::Undo(std::string* outError)
	{
		return Move(m_From, outError);
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetCreateAction
	////////////////////////////////////////////////////////////////////////////////

	AssetCreateAction::AssetCreateAction(EditorAssetManager& assets, std::string name, AssetHandle asset, std::string path)
		: AssetFileAction(assets, std::move(name)), m_Asset(asset), m_Path(std::move(path))
	{
	}

	bool AssetCreateAction::Execute(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		if (!assets)
			return false;
		if (!m_Trashed)
			return true; // Recorded once the asset was made
		if (!assets->RestoreFromTrash(*m_Trashed, outError))
			return false;
		m_Trashed.reset();
		return true;
	}

	bool AssetCreateAction::Undo(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		if (!assets)
			return false;
		// Only the asset this step made goes to the trash, never another one at its path.
		if (FindSourceAt(*assets, m_Path) != m_Asset)
			return Fail(outError, fmt::format("'{}' is no longer the asset this step created", m_Path));
		TrashedAssets trashed;
		if (!assets->MoveToTrash(m_Path, trashed, outError))
			return false;
		m_Trashed = std::move(trashed);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetFolderCreateAction
	////////////////////////////////////////////////////////////////////////////////

	AssetFolderCreateAction::AssetFolderCreateAction(EditorAssetManager& assets, std::string path)
		: AssetFileAction(assets, fmt::format("Create Folder {}", path)), m_Path(std::move(path))
	{
	}

	bool AssetFolderCreateAction::Execute(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		return assets && assets->CreateFolder(m_Path, outError);
	}

	bool AssetFolderCreateAction::Undo(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		return assets && assets->RemoveEmptyFolder(m_Path, outError);
	}

}
