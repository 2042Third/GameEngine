#include "Editor/AssetFileAction.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/Log.h>

namespace Strata
{

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

	AssetTrashAction::AssetTrashAction(EditorAssetManager& assets, std::string name, TrashedAssets trashed)
		: AssetFileAction(assets, std::move(name)), m_Trashed(std::move(trashed))
	{
	}

	bool AssetTrashAction::Execute(std::string* outError)
	{
		EditorAssetManager* assets = GetAssets(outError);
		// Each redo makes a new trash folder.
		return assets && assets->MoveToTrash(m_Trashed.Path, m_Trashed, outError);
	}

	void AssetTrashAction::Undo()
	{
		std::string error;
		EditorAssetManager* assets = GetAssets(&error);
		if (!assets || !assets->RestoreFromTrash(m_Trashed, &error))
			ST_ERROR("Undo of '{}' failed: {}", GetName(), error);
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

	void AssetMoveAction::Undo()
	{
		std::string error;
		if (!Move(m_From, &error))
			ST_ERROR("Undo of '{}' failed: {}", GetName(), error);
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetCreateAction
	////////////////////////////////////////////////////////////////////////////////

	AssetCreateAction::AssetCreateAction(EditorAssetManager& assets, std::string name, std::string path)
		: AssetFileAction(assets, std::move(name)), m_Path(std::move(path))
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

	void AssetCreateAction::Undo()
	{
		std::string error;
		EditorAssetManager* assets = GetAssets(&error);
		TrashedAssets trashed;
		if (!assets || !assets->MoveToTrash(m_Path, trashed, &error))
		{
			ST_ERROR("Undo of '{}' failed: {}", GetName(), error);
			return;
		}
		m_Trashed = std::move(trashed);
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

	void AssetFolderCreateAction::Undo()
	{
		std::string error;
		EditorAssetManager* assets = GetAssets(&error);
		if (!assets || !assets->RemoveEmptyFolder(m_Path, &error))
			ST_ERROR("Undo of '{}' failed: {}", GetName(), error);
	}

}
