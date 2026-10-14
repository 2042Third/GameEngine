#pragma once

#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/World/WorldStreamer.h>

#include <glm/glm.hpp>

#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	class Scene;
	class WorldAsset;

	// The world the editor edits: the edited scene is a world's persistent scene (its first active WorldComponent names a
	// registered World asset), and the cells around the editor camera are loaded into it as sub-scenes, edited in place and
	// saved by cell. EditorContext owns it (GetWorld; null while the edited scene is no world). Main thread.
	//
	// It streams with a WorldStreamer around the editor camera (each layer's distance times the radius scale; the visible
	// layers only), reading cells as their files are now. It keeps loaded the cells it must: dirty ones (until saved), those
	// holding selected entities, those world.loadCells pins and those an undo step asks for (ISceneCellLoader, until the next
	// save). As an UndoStackObserver it marks the documents each step touches dirty: the cells and the persistent scene of
	// the entities' states before and after, the persistent scene for settings, everything loaded for steps only a snapshot
	// describes. Dirty marks are conservative: undoing back to the saved state leaves them, and the save writes the same bytes.
	class EditorWorld final : public UndoStackObserver, public ISceneCellLoader
	{
	public:
		// The layer name new top-level entities go into by default: the persistent scene.
		static constexpr const char* c_PersistentLayer = "Persistent";

		// Attaches to `scene` when it is a world's persistent scene (null otherwise; a WorldComponent naming a World asset that
		// is not registered logs one warning). Registers with the undo stack and as the scene's ISceneCellLoader.
		static Scope<EditorWorld> Attach(EditorAssetManager& assets, Scene& scene, UndoStack& undo);
		~EditorWorld() override;

		EditorWorld(const EditorWorld&) = delete;
		EditorWorld& operator=(const EditorWorld&) = delete;

		Scene& GetScene() const { return m_Scene; }
		AssetHandle GetWorld() const { return m_World; }
		const WorldStreamer& GetStreamer() const { return m_Streamer; }
		// The World asset, once loaded (null before).
		Ref<WorldAsset> GetWorldAsset() const { return m_Streamer.GetWorldAsset(); }

		// How the editor edits the world (world.edit; for the session). The edit layer is a layer's name or c_PersistentLayer.
		const std::string& GetEditLayer() const { return m_EditLayer; }
		bool SetEditLayer(const std::string& layer);
		// The layers streamed (empty: every layer); false, changing nothing, for a name the world does not have.
		const std::vector<std::string>& GetVisibleLayers() const { return m_VisibleLayers; }
		bool SetVisibleLayers(std::vector<std::string> layers);
		float GetRadiusScale() const { return m_RadiusScale; }
		void SetRadiusScale(float scale);

		// Cells pinned by world.loadCells: loaded and kept until unpinned.
		void PinCell(const WorldCellKey& key);
		void UnpinCell(const WorldCellKey& key);
		void UnpinAllCells();
		const std::set<WorldCellKey>& GetPinnedCells() const { return m_Pinned; }

		// Documents changed since they were saved.
		bool IsPersistentDirty() const { return m_PersistentDirty; }
		const std::unordered_set<AssetHandle>& GetDirtyCells() const { return m_DirtyCells; }
		bool IsCellDirty(AssetHandle cell) const { return m_DirtyCells.contains(cell); }

		// The cell of a cell scene (the world's, or a new one's), and the reverse; nullopt / null when unknown.
		std::optional<WorldCellKey> FindCellKey(AssetHandle cell) const;
		AssetHandle FindCellScene(const WorldCellKey& key) const;
		// "Props (3, -1)".
		std::string DescribeKey(const WorldCellKey& key) const;

		// Once per frame in edit mode, before the scene's update (which advances the sub-scenes): streams around the editor
		// camera, keeping the cells it must.
		void Update(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, const std::vector<UUID>& selection);

		// UndoStackObserver
		void OnApplied(const EditorAction& action, bool undone, bool merged) override;
		// ISceneCellLoader
		std::string DescribeCell(AssetHandle cell) const override;
		void RequestCell(AssetHandle cell) override;
	private:
		EditorWorld(Scene& scene, UndoStack& undo, AssetHandle world);

		void MarkCellDirty(AssetHandle cell);
		void MarkStates(const std::vector<EntityState>& states);
		void ApplyVisibleLayers();
		// Pins in the streamer exactly the cells some reason keeps (world.loadCells, selection, undo, dirty).
		void SyncPins();
		void RefreshCellTable();

		Scene& m_Scene;
		UndoStack& m_Undo;
		AssetHandle m_World;
		WorldStreamer m_Streamer;
		std::weak_ptr<WorldAsset> m_TableWorld; // The World asset object the cell table was built from
		std::unordered_map<AssetHandle, WorldCellKey> m_CellKeys; // Cell scene -> key

		std::string m_EditLayer = c_PersistentLayer;
		std::vector<std::string> m_VisibleLayers;
		float m_RadiusScale = 1.0f;

		std::set<WorldCellKey> m_Pinned;              // world.loadCells
		std::set<WorldCellKey> m_SelectionPins;       // Cells of the selected entities
		std::set<WorldCellKey> m_RequestedPins;       // Undo steps' cells, until the next save
		std::unordered_set<AssetHandle> m_DirtyCells; // Pinned too
		std::set<WorldCellKey> m_StreamerPins;        // What the streamer has pinned now
		bool m_PersistentDirty = false;
		std::vector<WorldStreamingSource> m_Sources;  // Scratch
	};

}
