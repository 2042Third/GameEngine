#pragma once

#include "Editor/AssetReferences.h"

#include <Strata/Asset/AssetTypes.h>

#include <string>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;

	// The confirmation of the Content Browser's Delete: what goes (an asset, or a folder with its assets) and what uses it
	// (AssetReferences, found once when the dialog opens: every one listed, scrolling beyond a dozen), then asset.delete or
	// asset.deleteFolder, with force when something uses it ("Delete Anyway"). Deleting moves to the project's trash, which
	// Edit > Undo takes back from. Cancel or Escape closes it. Draw it every frame.
	class DeleteAssetDialog
	{
	public:
		void OpenForAsset(EditorContext& context, AssetHandle asset);
		void OpenForFolder(EditorContext& context, const std::string& folder);
		void Draw(EditorContext& context, const EditorCommandRegistry& commands);
		bool IsOpen() const { return m_Open; }

		// The modal's ImGui name (UI tests find its buttons as "DeleteAsset.Confirm" and "DeleteAsset.Cancel").
		static constexpr const char* c_PopupName = "Delete Asset##DeleteAssetDialog";
	private:
		// The lines that name each referencer, made when the dialog opens.
		void DescribeReferencers();
	private:
		bool m_Open = false;
		bool m_OpenRequested = false;
		bool m_Folder = false;
		AssetHandle m_Asset = UUID::Null();
		std::string m_Path;
		size_t m_AssetCount = 0;
		std::vector<AssetReferencer> m_Referencers;
		std::vector<std::string> m_ReferenceLines;
	};

}
