#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Asset/Asset.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;

	// Folders and assets of the project's asset directory. Assets are drag sources for the inspector (asset fields)
	// and the hierarchy (prefabs and models); files dropped onto the editor window are imported into the current
	// folder. The listing is refreshed periodically and after every action, not every frame.
	class ContentBrowserPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;

		// Opening a scene replaces the edited one: the editor decides how (e.g. after asking about unsaved changes).
		void SetOpenSceneHandler(std::function<void(AssetHandle)> handler) { m_OpenScene = std::move(handler); }

		// Imports external files into the current folder.
		void ImportFiles(EditorContext& context, const EditorCommandRegistry& commands, const std::vector<std::filesystem::path>& files);
		const std::string& GetCurrentDirectory() const { return m_CurrentDirectory; }
	private:
		struct Item
		{
			AssetMetadata Metadata;
			std::string Label;
			std::string FileName;
			std::string ImportError;
			std::string ImportWarnings;
		};

		void Refresh(EditorContext& context);
		void DrawToolbar(EditorContext& context, const EditorCommandRegistry& commands);
		void DrawRenamePopup(EditorContext& context, const EditorCommandRegistry& commands);
		void OpenScene(EditorContext& context, const EditorCommandRegistry& commands, AssetHandle scene);
	private:
		std::string m_CurrentDirectory; // Relative to the asset directory, '/' separators, empty for the root
		std::string m_Filter;
		std::function<void(AssetHandle)> m_OpenScene;

		std::vector<std::string> m_Folders;
		std::vector<Item> m_Items;
		double m_LastRefreshTime = -1.0;
		bool m_RefreshRequested = true;

		AssetHandle m_RenameAsset = UUID::Null();
		std::string m_RenameText;
		std::string m_NewFolderName;
	};

}
