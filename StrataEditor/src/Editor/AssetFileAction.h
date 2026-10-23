#pragma once

#include "Editor/UndoStack.h"

#include <Strata/Asset/EditorAssetManager.h>

#include <memory>
#include <optional>
#include <string>

namespace Strata
{

	class AssetManagerBase;
	class EditorContext;

	// Undo steps of operations on the project's files (asset.delete, asset.move, asset.createFolder, asset.moveFolder,
	// asset.deleteFolder). They share the history with scene edits but never change the scene, so they never mark it unsaved.
	// They refer to the project's asset manager weakly: once the project is closed (which clears the history) they do nothing.
	// Files can change outside the history (an asset created at a deleted one's path, a scene saved into a restored folder):
	// a step then fails rather than act on files that are no longer the ones it recorded.
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

	// Assets moved to the trash: undo restores them with their handles (failing while something else is at their path), redo
	// moves them there again when the path still holds exactly those assets and the command would delete them now (not the
	// open scene; nothing uses them unless the delete was forced). With `openSceneFromMemory` the open scene uses them only
	// when its entities in memory do (AssetReferences::IgnoreSavedOpenScene: a step that replaced the scene's use of them).
	class AssetTrashAction final : public AssetFileAction
	{
	public:
		AssetTrashAction(EditorContext& context, EditorAssetManager& assets, std::string name, TrashedAssets trashed, bool force,
			bool openSceneFromMemory = false);

		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		const TrashedAssets& GetTrashed() const { return m_Trashed; }
	private:
		bool CheckRedo(EditorAssetManager& assets, std::string* outError);
	private:
		EditorContext& m_Context; // Owns the history that owns this step
		TrashedAssets m_Trashed;
		bool m_Force;
		bool m_OpenSceneFromMemory;
	};

	// An asset moved or renamed (with a handle), or a folder moved (without one): undo moves it back.
	class AssetMoveAction final : public AssetFileAction
	{
	public:
		AssetMoveAction(EditorAssetManager& assets, std::string name, AssetHandle asset, std::string from, std::string to);

		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
	private:
		bool Move(const std::string& to, std::string* outError);
	private:
		AssetHandle m_Asset;
		std::string m_From;
		std::string m_To;
	};

	// An asset the editor created (a prefab from entities): undo moves it to the trash (failing when another asset is at its
	// path), redo brings it back with its handle.
	class AssetCreateAction final : public AssetFileAction
	{
	public:
		AssetCreateAction(EditorAssetManager& assets, std::string name, AssetHandle asset, std::string path);

		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
	private:
		AssetHandle m_Asset;
		std::string m_Path;
		std::optional<TrashedAssets> m_Trashed; // While undone
	};

	// A folder created: undo removes it while it is empty.
	class AssetFolderCreateAction final : public AssetFileAction
	{
	public:
		AssetFolderCreateAction(EditorAssetManager& assets, std::string path);

		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
	private:
		std::string m_Path;
	};

}
