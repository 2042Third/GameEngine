#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Asset/EditorAssetManager.h>

#include <memory>
#include <string>

namespace Strata
{

	class AssetManagerBase;

	// Undo steps of operations on the project's files (asset.delete, asset.move, asset.createFolder, asset.moveFolder,
	// asset.deleteFolder). They share the history with scene edits but never change the scene, so they never mark it unsaved.
	// They refer to the project's asset manager weakly: once the project is closed (which clears the history) they do nothing.
	class AssetFileAction : public EditorAction
	{
	public:
		AssetFileAction(EditorAssetManager& assets, std::string name);

		const std::string& GetName() const override { return m_Name; }
		bool ChangesScene() const override { return false; }
	protected:
		// The manager the step was recorded with, or null (with the reason in outError) when its project is closed.
		EditorAssetManager* GetAssets(std::string* outError) const;
	private:
		std::weak_ptr<AssetManagerBase> m_Assets;
		std::string m_Name;
	};

	// Assets moved to the trash: undo restores them with their handles, redo moves them there again.
	class AssetTrashAction final : public AssetFileAction
	{
	public:
		AssetTrashAction(EditorAssetManager& assets, std::string name, TrashedAssets trashed);

		bool Execute(std::string* outError) override;
		void Undo() override;
		const TrashedAssets& GetTrashed() const { return m_Trashed; }
	private:
		TrashedAssets m_Trashed;
	};

	// An asset moved or renamed (with a handle), or a folder moved (without one): undo moves it back.
	class AssetMoveAction final : public AssetFileAction
	{
	public:
		AssetMoveAction(EditorAssetManager& assets, std::string name, AssetHandle asset, std::string from, std::string to);

		bool Execute(std::string* outError) override;
		void Undo() override;
	private:
		bool Move(const std::string& to, std::string* outError);
	private:
		AssetHandle m_Asset;
		std::string m_From;
		std::string m_To;
	};

	// A folder created: undo removes it while it is empty.
	class AssetFolderCreateAction final : public AssetFileAction
	{
	public:
		AssetFolderCreateAction(EditorAssetManager& assets, std::string path);

		bool Execute(std::string* outError) override;
		void Undo() override;
	private:
		std::string m_Path;
	};

}
