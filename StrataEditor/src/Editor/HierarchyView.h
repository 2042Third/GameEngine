#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/Base.h>
#include <Strata/Core/UUID.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/Scene.h>

#include <entt/entt.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strata
{

	enum class HierarchyRowFlags : uint8_t
	{
		None = 0,
		HasChildren = ST_BIT(0), // The entity has children in the scene
		Expanded = ST_BIT(1),    // Rows of its children follow (while filtering: of those shown)
		Match = ST_BIT(2)        // While filtering: the entity matches (the other rows are ancestors shown for context)
	};
	ST_DEFINE_ENUM_FLAG_OPERATORS(HierarchyRowFlags)

	// One visible line of the hierarchy. The handle is valid until the scene's hierarchy changes, which makes the view
	// rebuild its rows at its next Update.
	struct HierarchyRow
	{
		entt::entity Handle = entt::null;
		UUID ID = UUID::Null();
		uint32_t Depth = 0; // In the scene (roots: 0), also while filtering
		HierarchyRowFlags Flags = HierarchyRowFlags::None;

		bool HasChildren() const { return HasFlag(Flags, HierarchyRowFlags::HasChildren); }
		bool IsExpanded() const { return HasFlag(Flags, HierarchyRowFlags::Expanded); }
		bool IsMatch() const { return HasFlag(Flags, HierarchyRowFlags::Match); }
	};

	// What the hierarchy's search box asks for: whitespace-separated terms that must all hold (case-insensitive). A plain
	// word is part of the name, "c:<text>" part of the name of a component the entity has (its registered or display name:
	// "c:light" finds every kind of light), "t:<text>" part of its tag. In a world, "l:<text>" is part of the name of the
	// layer whose cell holds the entity ("l:persistent": the persistent scene, the entities of no cell) and "cell:<x>,<z>"
	// the cell's coordinates; an entity belongs to the cell of its top-level ancestor.
	struct HierarchyFilter
	{
		std::vector<std::string> Words;      // Lower case
		std::vector<std::string> Components; // Lower case
		std::vector<std::string> Tags;       // Lower case
		std::vector<std::string> Layers;     // Lower case; c_PersistentLayerTerm names the persistent scene
		std::vector<std::pair<int32_t, int32_t>> Cells; // X, Z
		std::vector<std::string> InvalidCells; // "cell:" terms that are no "<x>,<z>": the filter then matches nothing

		static constexpr std::string_view c_PersistentLayerTerm = "persistent";

		static HierarchyFilter Parse(std::string_view text);
		bool IsEmpty() const { return Words.empty() && Components.empty() && Tags.empty() && !ReadsCells(); }
		// Whether a term asks for the entities' cells.
		bool ReadsCells() const { return !Layers.empty() || !Cells.empty() || !InvalidCells.empty(); }
	};

	// A world's cell as the hierarchy's "l:" and "cell:" terms see it.
	struct HierarchyCell
	{
		std::string Layer; // The layer's name
		int32_t X = 0;
		int32_t Z = 0;
	};
	// The cell whose scene a sub-scene was loaded from (SubSceneComponent::Source of its top-level entities), nullopt for a
	// scene that is no cell of the edited world (an additive scene a game loaded).
	using HierarchyCellResolver = std::function<std::optional<HierarchyCell>(AssetHandle scene)>;

	// Where on a row a dragged entity is dropped: the top quarter puts it before the row's entity, the bottom quarter after
	// it, the middle inside it (as its last child).
	enum class HierarchyDropZone : uint8_t
	{
		Before = 0,
		Inside,
		After
	};

	// What a drop does, in the terms of entity.setParent {entities, parent, index}.
	struct HierarchyDropTarget
	{
		HierarchyDropZone Zone = HierarchyDropZone::Inside;
		UUID Parent = UUID::Null(); // Null: the top level
		size_t Index = 0;           // Position the first dropped entity ends up at among the parent's children
	};

	// The hierarchy panel's model, without UI: the visible rows of the active scene as a flat list (depth-first, children of
	// expanded entities, or the entities a filter matches with their ancestors), the expansion state, the filter, revealing
	// an entity and the mapping of drop positions to hierarchy edits. Rows are rebuilt only when the scene, its hierarchy
	// version, the expansion or the filter changed, or (while filtering) a name, tag or filtered component did, so an
	// unchanged frame costs nothing however large the scene is; drawing only the visible rows (a list clipper) keeps
	// the panel's cost proportional to the window, not the scene. Main thread only.
	class HierarchyView final : private ComponentObserver
	{
	public:
		HierarchyView() = default;
		~HierarchyView() override;

		HierarchyView(const HierarchyView&) = delete;
		HierarchyView& operator=(const HierarchyView&) = delete;

		// Brings the rows up to date with `scene` (the editor's active scene, which changes with play mode and scene loads).
		// Call once per frame before reading rows. Returns true when they were rebuilt.
		bool Update(const Ref<Scene>& scene);

		const std::vector<HierarchyRow>& GetRows() const { return m_Rows; }
		// The row of an entity, when it is shown (a map built on the first call after a rebuild).
		std::optional<size_t> FindRow(UUID entity) const;
		// The entities of the rows from `first` to `last` (in either order, clamped to the rows): range selection.
		std::vector<UUID> GetRowRange(size_t first, size_t last) const;
		// How often the rows were rebuilt (so it also identifies the current rows).
		uint64_t GetRebuildCount() const { return m_RebuildCount; }

		// Expansion, kept per entity across scene loads and play mode (entities keep their IDs). New entities start collapsed.
		bool IsExpanded(UUID entity) const { return m_Expanded.contains(entity); }
		void SetExpanded(UUID entity, bool expanded);
		// The entity and all its descendants (Alt+click on an arrow).
		void SetSubtreeExpanded(const Scene& scene, UUID entity, bool expanded);
		void CollapseAll();

		// The search box's text (see HierarchyFilter); empty shows the hierarchy.
		void SetFilter(std::string_view text);
		const std::string& GetFilterText() const { return m_FilterText; }
		const HierarchyFilter& GetFilter() const { return m_Filter; }
		bool IsFiltering() const { return !m_Filter.IsEmpty(); }
		// Terms of "c:" that name no registered component (the filter then matches nothing): for a hint under the box.
		const std::vector<std::string>& GetUnknownComponents() const { return m_UnknownComponents; }
		// How "l:" and "cell:" terms find the cells of sub-scenes (the editor's world). Without one, no sub-scene is a cell:
		// "l:persistent" matches the entities of no sub-scene and the other cell terms nothing. A filter reading cells rebuilds
		// its rows when an entity's sub-scene changes; InvalidateCells rebuilds them when the resolver's answers change (another
		// world, renamed layers).
		void SetCellResolver(HierarchyCellResolver resolver);
		void InvalidateCells();

		// Makes an entity's row visible: expands its collapsed ancestors (outside filtering), updates the rows and returns
		// the ancestors it expanded (top-down) and the row to scroll to. No row when the entity does not exist or the filter
		// hides it.
		struct RevealResult
		{
			std::vector<UUID> Expanded;
			std::optional<size_t> Row;
		};
		RevealResult Reveal(const Ref<Scene>& scene, UUID entity);

		// The zone of a drop at `fraction` of a row's height from its top (0 to 1).
		static HierarchyDropZone GetDropZone(float fraction);
		// What dropping `dragged` at `fraction` of row `row` does; nullopt when the drop is impossible (onto a dragged entity
		// or one of its descendants, or no such row). Dropped after an expanded entity with shown children, the entities
		// become its first children (that is where the line between the rows lies). Index is the final position, so it
		// accounts for dragged entities that leave the same sibling list before it.
		std::optional<HierarchyDropTarget> GetDropTarget(const Scene& scene, size_t row, float fraction, std::span<const UUID> dragged) const;

		// Entities sorted in hierarchy order, without missing ones and without those below another listed one (they move,
		// copy and delete with it): what drag and drop, copy and duplicate pass to commands.
		static std::vector<UUID> GetTopmostInHierarchyOrder(const Scene& scene, std::span<const UUID> entities);
	private:
		void OnComponentChanged(entt::registry& registry, entt::entity entity) override;
		void Bind(const Ref<Scene>& scene);
		// Connects to the components the filter reads (only while filtering, so unfiltered frames pay nothing for edits).
		void ConnectFilterSignals();
		// Releases the connections while their scene lives; a scene that is gone took its signals with it.
		void Disconnect();
		void Rebuild(const Scene& scene);
		void BuildTree(const Scene& scene);
		void BuildFiltered(const Scene& scene);
		bool Matches(const entt::registry& registry, entt::entity entity) const;
		bool MatchesCell(const entt::registry& registry, entt::entity entity) const;
	private:
		std::weak_ptr<Scene> m_Scene;
		uint64_t m_HierarchyVersion = 0;
		bool m_Dirty = true;
		uint64_t m_RebuildCount = 0;

		std::vector<HierarchyRow> m_Rows;
		mutable std::unordered_map<UUID, size_t> m_RowIndex; // Built on demand
		mutable bool m_RowIndexValid = false;

		std::unordered_set<UUID> m_Expanded;

		std::string m_FilterText;
		HierarchyFilter m_Filter;
		std::vector<const ComponentInfo*> m_FilterComponents; // Per term of Components: the components it names, and the
		std::vector<size_t> m_FilterComponentTerms;           // index of the term each belongs to
		std::vector<std::string> m_UnknownComponents;
		std::vector<entt::connection> m_Connections; // To m_Scene's registry
		std::vector<uint8_t> m_Marks; // Scratch of BuildFiltered, by entity index
		HierarchyCellResolver m_CellResolver;
		// The resolver's answers of one rebuild, by cell scene (layer names lower case).
		mutable std::unordered_map<AssetHandle, std::optional<HierarchyCell>> m_ResolvedCells;
	};

}
