#pragma once

#include "Editor/DocumentSaver.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Scene/Scene.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldStreamer.h>

#include <glm/glm.hpp>

#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	class WorldAsset;

	// A document an EditorWorld save wrote (scene.save's documents).
	struct WorldSavedDocument
	{
		std::string Path;     // Relative to the asset directory
		std::string Kind;     // "persistent", "cell" or "world"
		size_t Bytes = 0;
		uint64_t Entities = 0;
	};

	// What one save of a world writes (EditorWorld::BeginSave): the items of its DocumentSaver request, in the order of its
	// documents, and what the completions need of each.
	struct WorldSavePlan
	{
		struct Document
		{
			std::string Kind; // "persistent" or "cell"
			AssetHandle Asset = UUID::Null();
			std::string Path;
			WorldCellKey Key;  // Cells
			bool New = false;  // A cell made in memory, reserved at Path with its derived handle
			// Cells: measured from memory as the save began; its stamp and closure bytes are taken once it is written.
			WorldCellSummary Summary;
		};
		std::vector<DocumentSaveItem> Items;
		std::vector<Document> Documents;
		bool PersistentDirty = false; // The persistent scene was dirty (it is written whenever it is, or when nothing else is)
	};

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
		// Changes whenever a cell becomes dirty or is saved (the cell map redraws its outlines then).
		uint64_t GetDirtyVersion() const { return m_DirtyVersion; }

		// Puts the top-level entities an edit tracks into the cells their positions lie in, within the edit (so one undo step
		// reverses it): an entity of a cell moves to the cell of its layer at its world position (a gizmo drag or a translation
		// across a border, a paste); an entity the edit created that belongs to no sub-scene goes into the edit layer's cell at
		// its position (the persistent scene for c_PersistentLayer). A target cell that does not exist is made in memory
		// (written at the next save); one that is not loaded is loaded and pinned, and the entity moves with its next edit.
		void Recell(const SceneEditTransaction& transaction);
		// Moves top-level entities into the cell of a layer at their positions, or into the persistent scene for
		// c_PersistentLayer (world.moveEntities). False with the reason, changing nothing, for an unknown layer, an entity that
		// is not top-level or a target cell that is not loaded yet (it is loading then).
		bool MoveEntities(const std::vector<Entity>& entities, const std::string& layer, std::string* outError = nullptr);

		// Readies the play copy of the edited scene (Scene::Copy) before it starts: the cells loaded in the editor become its
		// sub-scenes (adopted by their roots' IDs), and the entities of cells still loading or unloading leave the copy, so
		// that the game's streamer loads those whole from their documents. Returns the cells with unsaved edits, which the
		// game's World system keeps loaded for the session (WorldStreamingSystem::PinCell).
		std::vector<WorldCellKey> PrepareRuntimeCopy(Scene& copy) const;

		// How the cells that recovered changes name stand (PrepareRecoveredCells).
		enum class RecoveredCells : uint8_t
		{
			Ready,   // Every cell the changes name is loaded (or made in memory)
			Loading, // Some still load: ask again next frame
			Failed   // The World asset or a cell cannot be loaded
		};
		// The cells entity states name, each once, in the order they first appear.
		static std::vector<AssetHandle> CollectCells(std::span<const std::vector<EntityState>> steps);
		// scene.recover of a world, every frame until it is Ready before its changes apply: loads and keeps (until the next
		// save) the cells the recovered states name (`cells`, CollectCells of `steps`), so that the changes go into the cells
		// and meet the entities the cells hold, and makes in memory the cells the world does not have (made after the last
		// save), finding each from the position the states record for one of its top-level entities. A cell no recorded
		// position finds stays out, and its entities are recovered into the persistent scene (ApplyEntities warns). Takes
		// the cells it is done with out of `cells` (Ready when none is left), so that waiting allocates nothing.
		RecoveredCells PrepareRecoveredCells(std::vector<AssetHandle>& cells, std::span<const std::vector<EntityState>> steps,
			std::string* outError = nullptr);

		// A save of the documents changed since the last save, in two DocumentSaver requests (EditorContext::BeginSaveScene):
		// - BeginSave plans the first, written from a snapshot of the edited scene: the persistent scene's entities without the
		//   cells' (with the scene's settings; written when it is dirty, or when nothing is), and each dirty cell's top-level
		//   entities with their descendants (no settings), new cells reserved with their derived handles. The cells' index
		//   entries are measured from memory then. The dirty marks clear at once, edits made meanwhile mark again, and the
		//   saved cells stay loaded until the save ended. False, with the reason and nothing changed, while the World asset
		//   loads, a previous save still writes or a new cell's path cannot be reserved.
		// - FinishSave takes the first request's outcome: documents that failed are dirty again; for the cells written it
		//   refreshes their index entries (stamps, closure bytes) and lists the new ones, and returns the World document to
		//   write next (Document bytes), nullopt when it is unchanged (the save is done then) or cannot be written (outError:
		//   invalid, or the World asset no longer loaded). Until that document is written the scene is not saved
		//   (EditorContext marks it then), and the next save lists the new cells.
		// - FinishIndexSave ends the save once that document's write ended.
		// CancelSave undoes BeginSave when the request could not be submitted.
		bool BeginSave(EditorAssetManager& assets, AssetHandle persistent, WorldSavePlan& outPlan, std::string* outError = nullptr);
		void CancelSave(EditorAssetManager& assets, const WorldSavePlan& plan);
		std::optional<std::string> FinishSave(EditorAssetManager& assets, const WorldSavePlan& plan, const DocumentSaveStatus& status,
			std::string* outError = nullptr);
		void FinishIndexSave();
		// A save began and has not ended (BeginSave until FinishSave without a document, or FinishIndexSave).
		bool IsSaving() const { return m_Saving; }

		// The cell of a cell scene (the world's, or a new one's), and the reverse; nullopt / null when unknown.
		std::optional<WorldCellKey> FindCellKey(AssetHandle cell) const;
		AssetHandle FindCellScene(const WorldCellKey& key) const;
		// "Props (3, -1)".
		std::string DescribeKey(const WorldCellKey& key) const;

		// Once per frame in edit mode, before the scene's update (which advances the sub-scenes): streams around the editor
		// camera, keeping the cells it must. The selected entities' cells (`selection`, whose version is `selectionVersion`,
		// EditorContext::GetSelectionVersion) are looked up again only when the selection, the scene's hierarchy or
		// sub-scene membership, or the cell table changed.
		void Update(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, const std::vector<UUID>& selection, uint64_t selectionVersion);
		// How many updates looked up the selected entities' cells (tests: frames that change nothing look nothing up).
		uint64_t GetSelectionPinLookups() const { return m_SelectionPinLookups; }

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
		// The loaded sub-scene of a cell, made in memory when the cell does not exist yet (with its derived handle); 0 when it
		// exists but is not loaded (it is loading then).
		SubSceneID EnsureCellSubScene(const WorldCellKey& key);
		// The cell of a layer at an entity's world position.
		WorldCellKey GetCellAt(const WorldLayer& layer, const Entity& entity) const;
		// The scene asset a cell that does not exist yet gets (derived from the world's handle: the same on every machine).
		AssetHandle DeriveCellScene(const WorldCellKey& key) const;
		// The key of a cell the world does not have, from the position `steps` record for a top-level entity of it.
		std::optional<WorldCellKey> FindRecordedCell(AssetHandle cell, std::span<const std::vector<EntityState>> steps) const;

		Scene& m_Scene;
		UndoStack& m_Undo;
		AssetHandle m_World;
		WorldStreamer m_Streamer;
		std::weak_ptr<WorldAsset> m_TableWorld; // The World asset object the cell table was built from
		std::unordered_map<AssetHandle, WorldCellKey> m_CellKeys; // Cell scene -> key (the world's cells and the new ones)
		std::unordered_map<AssetHandle, WorldCellKey> m_NewCells; // Cells made in memory, written into the world at the next save

		std::string m_EditLayer = c_PersistentLayer;
		std::vector<std::string> m_VisibleLayers;
		float m_RadiusScale = 1.0f;

		std::set<WorldCellKey> m_Pinned;              // world.loadCells
		// What the selection's cells were looked up for: the selection, hierarchy, membership and cell table versions.
		struct SelectionPinsKey
		{
			uint64_t Selection = 0;
			uint64_t Hierarchy = 0;
			uint64_t Membership = 0;
			uint64_t CellTable = 0;

			bool operator==(const SelectionPinsKey&) const = default;
		};

		std::set<WorldCellKey> m_SelectionPins;       // Cells of the selected entities
		std::optional<SelectionPinsKey> m_SelectionPinsKey; // None before the first lookup
		uint64_t m_SelectionPinLookups = 0;
		uint64_t m_CellTableVersion = 0;              // Changes with m_CellKeys
		std::set<WorldCellKey> m_RequestedPins;       // Undo steps' cells, until the next save
		std::unordered_set<AssetHandle> m_DirtyCells; // Pinned too
		uint64_t m_DirtyVersion = 0;
		std::set<WorldCellKey> m_StreamerPins;        // What the streamer has pinned now
		bool m_PersistentDirty = false;
		bool m_Saving = false;
		std::unordered_set<AssetHandle> m_SavingCells; // Cells a save writes: pinned until it ended
		SceneSettings m_SavedSettings;                // The persistent scene's settings as saved: any step that changes them marks it
		std::vector<WorldStreamingSource> m_Sources;  // Scratch
	};

}
