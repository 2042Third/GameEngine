#include "Editor/EditorWorld.h"

#include "Editor/WorldCellIO.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldComponents.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// The scene's first active WorldComponent in hierarchy order; an invalid entity without one.
		Entity FindWorldEntity(Scene& scene)
		{
			Entity found;
			const entt::registry& registry = scene.GetRegistry();
			for (const entt::entity handle : registry.view<const WorldComponent>())
			{
				const Entity entity(handle, &scene);
				if (scene.IsActiveInHierarchy(entity) && (!found || scene.CompareHierarchyOrder(entity, found) < 0))
					found = entity;
			}
			return found;
		}

		// The translation an entity state records (that of a top-level entity is its world position).
		std::optional<glm::vec3> ReadTranslation(const nlohmann::json& components)
		{
			const nlohmann::json* transform = JsonUtils::Find(components, "Transform");
			const nlohmann::json* translation = transform ? JsonUtils::Find(*transform, "Translation") : nullptr;
			if (!translation || !translation->is_array() || translation->size() != 3)
				return std::nullopt;
			glm::vec3 position(0.0f);
			for (int axis = 0; axis < 3; axis++)
			{
				const nlohmann::json& value = (*translation)[static_cast<size_t>(axis)];
				if (!value.is_number() || !std::isfinite(value.get<double>()))
					return std::nullopt;
				position[axis] = value.get<float>();
			}
			return position;
		}

	}

	Scope<EditorWorld> EditorWorld::Attach(EditorAssetManager& assets, Scene& scene, UndoStack& undo)
	{
		const Entity entity = FindWorldEntity(scene);
		if (!entity)
			return nullptr;
		const AssetHandle world = entity.GetComponent<WorldComponent>().World;
		if (!world.IsValid())
			return nullptr;
		const std::optional<AssetMetadata> metadata = assets.GetMetadata(world);
		if (!metadata || metadata->Type != AssetType::World)
		{
			ST_WARN("'{}' names the world {}, which is not a world of the project: it is edited as a plain scene", scene.GetName(), world.ToString());
			return nullptr;
		}
		return Scope<EditorWorld>(new EditorWorld(scene, undo, world));
	}

	EditorWorld::EditorWorld(Scene& scene, UndoStack& undo, AssetHandle world)
		: m_Scene(scene), m_Undo(undo), m_World(world), m_Streamer(scene)
	{
		m_Streamer.SetWorld(world, m_RadiusScale);
		// The editor reads cells as their files are now, not as a loaded copy remembers them.
		m_Streamer.SetReloadCells(true);
		m_Undo.AddObserver(this);
		m_Scene.RegisterService<ISceneCellLoader>(*this);
		m_SavedSettings = m_Scene.GetSettings();
	}

	EditorWorld::~EditorWorld()
	{
		m_Scene.UnregisterService<ISceneCellLoader>(*this);
		m_Undo.RemoveObserver(this);
	}

	bool EditorWorld::SetEditLayer(const std::string& layer)
	{
		if (layer != c_PersistentLayer)
		{
			const Ref<WorldAsset> world = GetWorldAsset();
			if (!world || !world->GetData().FindLayer(layer))
				return false;
		}
		m_EditLayer = layer;
		return true;
	}

	bool EditorWorld::SetVisibleLayers(std::vector<std::string> layers)
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return false;
		for (const std::string& layer : layers)
		{
			if (!world->GetData().FindLayer(layer))
				return false;
		}
		m_VisibleLayers = std::move(layers);
		ApplyVisibleLayers();
		return true;
	}

	void EditorWorld::ApplyVisibleLayers()
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return;
		for (const WorldLayer& layer : world->GetData().Layers)
		{
			const bool visible = m_VisibleLayers.empty() || std::find(m_VisibleLayers.begin(), m_VisibleLayers.end(), layer.Name) != m_VisibleLayers.end();
			m_Streamer.SetLayerEnabled(layer.Name, visible);
		}
	}

	void EditorWorld::SetRadiusScale(float scale)
	{
		m_RadiusScale = std::isfinite(scale) ? std::clamp(scale, 0.1f, 10.0f) : 1.0f;
		m_Streamer.SetWorld(m_World, m_RadiusScale);
	}

	void EditorWorld::PinCell(const WorldCellKey& key)
	{
		m_Pinned.insert(key);
	}

	void EditorWorld::UnpinCell(const WorldCellKey& key)
	{
		m_Pinned.erase(key);
	}

	void EditorWorld::UnpinAllCells()
	{
		m_Pinned.clear();
	}

	void EditorWorld::RefreshCellTable()
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world || m_TableWorld.lock() == world)
			return;
		m_TableWorld = world;
		m_CellTableVersion++;
		m_CellKeys.clear();
		for (const WorldCell& cell : world->GetData().Cells)
			m_CellKeys.emplace(cell.Scene, cell.GetKey());
		// Cells made in memory stay new until the world lists them.
		std::erase_if(m_NewCells, [&](const auto& item) { return m_CellKeys.contains(item.first); });
		for (const auto& [cell, key] : m_NewCells)
			m_CellKeys.emplace(cell, key);
		ApplyVisibleLayers();
	}

	WorldCellKey EditorWorld::GetCellAt(const WorldLayer& layer, const Entity& entity) const
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		const glm::vec3 position(m_Scene.GetWorldTransform(entity)[3]);
		return WorldGrid::CellAt(layer, world ? world->GetData().GridOrigin : glm::dvec2(0.0), glm::dvec3(position));
	}

	AssetHandle EditorWorld::DeriveCellScene(const WorldCellKey& key) const
	{
		return DeriveSubAssetHandle(m_World, fmt::format("Cell/{}/{}/{}", key.Layer, key.X, key.Z));
	}

	std::optional<WorldCellKey> EditorWorld::FindRecordedCell(AssetHandle cell, std::span<const std::vector<EntityState>> steps) const
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return std::nullopt;
		for (const std::vector<EntityState>& step : steps)
		{
			for (const EntityState& state : step)
			{
				if (state.Cell != cell || !state.Exists || state.Parent.IsValid())
					continue;
				const std::optional<glm::vec3> position = ReadTranslation(state.Components);
				if (!position)
					continue;
				for (const WorldLayer& layer : world->GetData().Layers)
				{
					const WorldCellKey key = WorldGrid::CellAt(layer, world->GetData().GridOrigin, glm::dvec3(*position));
					if (DeriveCellScene(key) == cell)
						return key;
				}
			}
		}
		return std::nullopt;
	}

	std::vector<AssetHandle> EditorWorld::CollectCells(std::span<const std::vector<EntityState>> steps)
	{
		std::vector<AssetHandle> cells;
		std::unordered_set<AssetHandle> seen;
		for (const std::vector<EntityState>& step : steps)
		{
			for (const EntityState& state : step)
			{
				if (state.Cell.IsValid() && seen.insert(state.Cell).second)
					cells.push_back(state.Cell);
			}
		}
		return cells;
	}

	EditorWorld::RecoveredCells EditorWorld::PrepareRecoveredCells(std::vector<AssetHandle>& cells, std::span<const std::vector<EntityState>> steps,
		std::string* outError)
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
		{
			const AssetManagerBase* assets = m_Scene.GetAssetManager();
			if (assets && assets->GetAssetState(m_World) == AssetState::Failed)
			{
				if (outError)
					*outError = fmt::format("The world {} cannot be loaded, so the changes cannot go back into its cells", m_World.ToString());
				return RecoveredCells::Failed;
			}
			return RecoveredCells::Loading;
		}
		RefreshCellTable();
		bool pinned = false;
		for (size_t index = cells.size(); index-- > 0;)
		{
			const AssetHandle cell = cells[index];
			const std::optional<WorldCellKey> key = FindCellKey(cell);
			// Kept from now on, also when it is loaded already: the camera may move away before the others are in.
			if (key)
				pinned |= m_RequestedPins.insert(*key).second;
			bool done = m_Scene.IsSubSceneLoaded(m_Scene.FindSubSceneBySource(cell));
			if (!done && !key)
			{
				// A cell made after the last save: no document holds it, so it is made again.
				if (const std::optional<WorldCellKey> recorded = FindRecordedCell(cell, steps))
					EnsureCellSubScene(*recorded);
				done = true;
			}
			if (done)
			{
				cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(index));
				continue;
			}
			std::string error;
			if (m_Streamer.GetCellStatus(*key, &error) == WorldCellStatus::Failed)
			{
				if (outError)
					*outError = fmt::format("The cell {} cannot be loaded ({}), so the changes to it cannot be recovered; fix it and run scene.recover again",
						DescribeKey(*key), error);
				return RecoveredCells::Failed;
			}
		}
		if (pinned)
			SyncPins();
		return cells.empty() ? RecoveredCells::Ready : RecoveredCells::Loading;
	}

	SubSceneID EditorWorld::EnsureCellSubScene(const WorldCellKey& key)
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return 0;
		AssetHandle cell = FindCellScene(key);
		const bool known = cell.IsValid();
		for (const auto& [handle, newKey] : m_NewCells)
		{
			if (newKey == key)
				cell = handle;
		}
		if (cell.IsValid())
		{
			const SubSceneID id = m_Scene.FindSubSceneBySource(cell);
			if (m_Scene.IsSubSceneLoaded(id))
				return id;
			if (known)
			{
				// It exists but is not here: it loads (and stays) for the edit to come.
				m_RequestedPins.insert(key);
				return 0;
			}
		}
		// A cell that does not exist yet: made in memory with its derived handle, written at the next save.
		const WorldLayer* layer = world->GetData().FindLayer(key.Layer);
		const WorldCellSquare square = WorldGrid::CellBounds(*layer, world->GetData().GridOrigin, key.X, key.Z);
		if (std::max({ std::abs(square.Min.x), std::abs(square.Min.y), std::abs(square.Max.x), std::abs(square.Max.y) }) > WorldData::c_MaxExtent)
			return 0;
		cell = DeriveCellScene(key);
		const SubSceneID id = m_Scene.AdoptSubScene(cell, {});
		if (id == 0)
			return 0;
		m_NewCells[cell] = key;
		m_CellKeys[cell] = key;
		m_CellTableVersion++;
		MarkCellDirty(cell);
		return id;
	}

	void EditorWorld::Recell(const SceneEditTransaction& transaction)
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return;
		RefreshCellTable();
		const WorldData& data = world->GetData();
		const WorldLayer* editLayer = m_EditLayer == c_PersistentLayer ? nullptr : data.FindLayer(m_EditLayer);
		const std::vector<UUID>& entities = transaction.GetEntities();
		for (size_t index = 0; index < entities.size(); index++)
		{
			const Entity entity = m_Scene.GetEntityByUUID(entities[index]);
			if (!entity || entity.GetComponent<RelationshipComponent>().Parent.IsValid())
				continue;
			const SubSceneID current = m_Scene.GetSubSceneOf(entity);
			const SubSceneStatus* status = m_Scene.GetSubSceneStatus(current);
			const std::optional<WorldCellKey> currentKey = status ? FindCellKey(status->Scene) : std::nullopt;
			const WorldLayer* layer = nullptr;
			if (currentKey)
				layer = data.FindLayer(currentKey->Layer);
			else if (transaction.WasCreated(index) && current == 0)
				layer = editLayer;
			if (!layer)
				continue;
			const WorldCellKey target = GetCellAt(*layer, entity);
			if (currentKey && target == *currentKey)
				continue;
			if (const SubSceneID id = EnsureCellSubScene(target))
				m_Scene.SetSubSceneOf(entity, id);
		}
	}

	bool EditorWorld::MoveEntities(const std::vector<Entity>& entities, const std::string& layerName, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return fail("The world is still loading; try again in a moment");
		RefreshCellTable();
		const WorldLayer* layer = layerName == c_PersistentLayer ? nullptr : world->GetData().FindLayer(layerName);
		if (layerName != c_PersistentLayer && !layer)
			return fail(fmt::format("The world has no layer '{}'", layerName));
		for (const Entity& entity : entities)
		{
			if (entity.GetComponent<RelationshipComponent>().Parent.IsValid())
				return fail(fmt::format("'{}' is not a top-level entity: entities belong to their top-level ancestor's cell", entity.GetName()));
		}
		// Every target cell first, so that nothing moves when one is not there yet.
		std::vector<WorldCellKey> targets;
		if (layer)
		{
			for (const Entity& entity : entities)
			{
				const WorldCellKey key = GetCellAt(*layer, entity);
				const AssetHandle cell = FindCellScene(key);
				if (cell.IsValid() && !m_Scene.IsSubSceneLoaded(m_Scene.FindSubSceneBySource(cell)))
				{
					m_RequestedPins.insert(key);
					return fail(fmt::format("The cell {} is not loaded; it is loading now, try again once it is in", DescribeKey(key)));
				}
				targets.push_back(key);
			}
		}
		for (size_t index = 0; index < entities.size(); index++)
		{
			const SubSceneID id = layer ? EnsureCellSubScene(targets[index]) : 0;
			if (layer && id == 0)
				return fail(fmt::format("'{}' lies outside the world", entities[index].GetName()));
			m_Scene.SetSubSceneOf(entities[index], id);
		}
		return true;
	}

	std::optional<WorldCellKey> EditorWorld::FindCellKey(AssetHandle cell) const
	{
		const auto it = m_CellKeys.find(cell);
		return it != m_CellKeys.end() ? std::optional<WorldCellKey>(it->second) : std::nullopt;
	}

	AssetHandle EditorWorld::FindCellScene(const WorldCellKey& key) const
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return UUID::Null();
		const int32_t index = world->FindCell(key);
		return index >= 0 ? world->GetData().Cells[static_cast<size_t>(index)].Scene : UUID::Null();
	}

	std::string EditorWorld::DescribeKey(const WorldCellKey& key) const
	{
		const Ref<WorldAsset> world = GetWorldAsset();
		const WorldLayer* layer = world ? world->GetData().FindLayer(key.Layer) : nullptr;
		return fmt::format("{} ({}, {})", layer ? layer->Name : fmt::format("layer {}", key.Layer), key.X, key.Z);
	}

	std::string EditorWorld::DescribeCell(AssetHandle cell) const
	{
		const std::optional<WorldCellKey> key = FindCellKey(cell);
		return key ? DescribeKey(*key) : cell.ToString();
	}

	void EditorWorld::RequestCell(AssetHandle cell)
	{
		if (const std::optional<WorldCellKey> key = FindCellKey(cell))
			m_RequestedPins.insert(*key);
	}

	void EditorWorld::MarkCellDirty(AssetHandle cell)
	{
		if (m_DirtyCells.insert(cell).second)
			m_DirtyVersion++;
	}

	void EditorWorld::MarkStates(const std::vector<EntityState>& states)
	{
		// A state names the document of its entity (its top-level ancestor's; for an entity that does not exist, the one it
		// left or goes into).
		for (const EntityState& state : states)
		{
			if (state.Cell.IsValid())
				MarkCellDirty(state.Cell);
			else
				m_PersistentDirty = true;
		}
	}

	void EditorWorld::OnApplied(const EditorAction& action, bool undone, bool)
	{
		// Settings belong to the persistent scene, whatever kind of step changed them (a recovery's changes them with its
		// entities).
		if (!(m_Scene.GetSettings() == m_SavedSettings))
			m_PersistentDirty = true;
		switch (action.GetSceneChange())
		{
			case SceneChangeKind::None:
				return;
			case SceneChangeKind::Settings:
				m_PersistentDirty = true;
				return;
			case SceneChangeKind::Entities:
				// Both directions: an entity moved from one cell to another changes both documents.
				if (const Ref<const std::vector<EntityState>> now = action.GetEntityStates(undone))
					MarkStates(*now);
				if (const Ref<const std::vector<EntityState>> other = action.GetEntityStates(!undone))
					MarkStates(*other);
				return;
			case SceneChangeKind::Other:
				m_PersistentDirty = true;
				for (const SubSceneStatus& status : m_Scene.GetSubScenes())
				{
					if (status.State == SubSceneState::Loaded && status.Scene.IsValid())
						MarkCellDirty(status.Scene);
				}
				return;
		}
	}

	void EditorWorld::SyncPins()
	{
		std::set<WorldCellKey> wanted = m_Pinned;
		wanted.insert(m_SelectionPins.begin(), m_SelectionPins.end());
		wanted.insert(m_RequestedPins.begin(), m_RequestedPins.end());
		for (const std::unordered_set<AssetHandle>* cells : { &m_DirtyCells, &m_SavingCells })
		{
			for (const AssetHandle cell : *cells)
			{
				if (const std::optional<WorldCellKey> key = FindCellKey(cell))
					wanted.insert(*key);
			}
		}
		if (wanted == m_StreamerPins)
			return;
		for (const WorldCellKey& key : m_StreamerPins)
		{
			if (!wanted.contains(key))
				m_Streamer.UnpinCell(key);
		}
		for (const WorldCellKey& key : wanted)
		{
			if (!m_StreamerPins.contains(key))
				m_Streamer.PinCell(key);
		}
		m_StreamerPins = std::move(wanted);
	}

	void EditorWorld::Update(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, const std::vector<UUID>& selection, uint64_t selectionVersion)
	{
		RefreshCellTable();
		// The cells of the selected entities stay while they are selected. Looking them up costs a walk to each entity's
		// top-level ancestor (a select-all of a world selects every loaded entity): only after something they depend on changed.
		const SelectionPinsKey pinsKey { selectionVersion, m_Scene.GetHierarchyVersion(), m_Scene.GetSubSceneMembershipVersion(), m_CellTableVersion };
		if (m_SelectionPinsKey != pinsKey)
		{
			m_SelectionPinsKey = pinsKey;
			m_SelectionPinLookups++;
			m_SelectionPins.clear();
			for (const UUID id : selection)
			{
				const Entity entity = m_Scene.GetEntityByUUID(id);
				const SubSceneStatus* status = entity ? m_Scene.GetSubSceneStatus(m_Scene.GetSubSceneOf(entity)) : nullptr;
				if (status && status->Scene.IsValid())
				{
					if (const std::optional<WorldCellKey> key = FindCellKey(status->Scene))
						m_SelectionPins.insert(*key);
				}
			}
		}
		SyncPins();

		WorldStreamingSource source;
		source.Position = cameraPosition;
		source.Forward = cameraForward;
		m_Sources.assign(1, source);
		m_Streamer.SetSources(m_Sources);
		m_Streamer.Update();
	}

	bool EditorWorld::BeginSave(EditorAssetManager& assets, AssetHandle persistent, WorldSavePlan& outPlan, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		outPlan = {};
		if (m_Saving)
			return fail("The world's previous save is still being written: save again once it ended");
		RefreshCellTable();
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return fail("The world is still loading: save again in a moment");

		// The top-level entities by document, in hierarchy order: a cell's belong to its sub-scene, the rest to the persistent
		// scene (also those of other additive loads, as a plain scene's save keeps them).
		std::vector<UUID> persistentRoots;
		std::unordered_map<AssetHandle, std::vector<Entity>> cellRoots;
		for (const Entity entity : m_Scene.GetEntitiesInHierarchyOrder())
		{
			if (entity.GetComponent<RelationshipComponent>().Parent.IsValid())
				continue;
			const SubSceneStatus* status = m_Scene.GetSubSceneStatus(m_Scene.GetSubSceneOf(entity));
			if (status && status->Scene.IsValid() && m_CellKeys.contains(status->Scene))
				cellRoots[status->Scene].push_back(entity);
			else
				persistentRoots.push_back(entity.GetUUID());
		}
		const std::optional<AssetMetadata> persistentMetadata = assets.GetMetadata(persistent);
		if (!persistentMetadata)
			return fail("The persistent scene has no asset of the project");
		const std::optional<AssetMetadata> worldMetadata = assets.GetMetadata(m_World);
		if (!worldMetadata)
			return fail("The world has no asset of the project");

		// The dirty cells, in key order.
		std::vector<std::pair<WorldCellKey, AssetHandle>> dirty;
		for (const AssetHandle cell : m_DirtyCells)
		{
			if (const std::optional<WorldCellKey> key = FindCellKey(cell))
				dirty.emplace_back(*key, cell);
		}
		std::sort(dirty.begin(), dirty.end());

		WorldSavePlan plan;
		plan.PersistentDirty = m_PersistentDirty;
		if (m_PersistentDirty || dirty.empty())
		{
			DocumentSaveItem item;
			item.Asset = persistent;
			item.Write.Roots = std::move(persistentRoots);
			item.Write.WriteSettings = true;
			item.Write.Name = m_Scene.GetName();
			plan.Items.push_back(std::move(item));
			plan.Documents.push_back({ "persistent", persistent, persistentMetadata->Path });
		}
		const WorldData& data = world->GetData();
		std::vector<AssetHandle> reserved;
		for (const auto& [key, cell] : dirty)
		{
			const auto roots = cellRoots.find(cell);
			const std::vector<Entity> entities = roots != cellRoots.end() ? roots->second : std::vector<Entity>();
			WorldSavePlan::Document document { "cell", cell, std::string(), key };
			DocumentSaveItem item;
			item.Asset = cell;
			item.Write.WriteSettings = false;
			// A cell made in memory gets its document at <world folder>/<world name>/<layer>/<X>_<Z>.stscene, with its derived
			// handle (the save writes its .meta); one whose document a save wrote is an asset already (the World document
			// lists it at the next save).
			if (m_NewCells.contains(cell) && !assets.GetMetadata(cell))
			{
				const WorldLayer* layer = data.FindLayer(key.Layer);
				if (!layer)
				{
					for (const AssetHandle handle : reserved)
						assets.ReleaseNativeAssetReservation(handle);
					return fail(fmt::format("The new cell {} belongs to no layer of the world", DescribeKey(key)));
				}
				const std::filesystem::path worldPath = FileSystem::FromUTF8(worldMetadata->Path);
				// Asset paths use '/' on every system.
				const std::string folder = FileSystem::ToUTF8(worldPath.parent_path());
				document.Path = fmt::format("{}{}{}/{}/{}_{}.stscene", folder, folder.empty() ? "" : "/", FileSystem::ToUTF8(worldPath.stem()), layer->Name, key.X, key.Z);
				std::replace(document.Path.begin(), document.Path.end(), '\\', '/');
				std::string error;
				if (assets.ReserveNativeAsset(document.Path, cell, &error) != cell)
				{
					for (const AssetHandle handle : reserved)
						assets.ReleaseNativeAssetReservation(handle);
					return fail(fmt::format("The new cell {} cannot be saved: {}", DescribeKey(key), error));
				}
				reserved.push_back(cell);
				document.New = true;
				item.NewPath = document.Path;
				item.Write.Name = fmt::format("{}_{}", key.X, key.Z);
			}
			else
			{
				const std::optional<AssetMetadata> metadata = assets.GetMetadata(cell);
				if (!metadata)
				{
					for (const AssetHandle handle : reserved)
						assets.ReleaseNativeAssetReservation(handle);
					return fail(fmt::format("The cell {} has no asset of the project", DescribeKey(key)));
				}
				document.Path = metadata->Path;
				item.Write.Name = metadata->Name;
			}
			for (const Entity& root : entities)
				item.Write.Roots.push_back(root.GetUUID());
			document.Summary = WorldCellIO::SummarizeEntities(m_Scene, entities, assets, UUID::Null()).Summary;
			document.Summary.Layer = key.Layer;
			document.Summary.X = key.X;
			document.Summary.Z = key.Z;
			plan.Items.push_back(std::move(item));
			plan.Documents.push_back(std::move(document));
		}

		// The marks clear now: edits made while the save writes mark the documents again. The cells it writes stay loaded
		// until it ended, so that one that fails is still there to save again.
		for (const auto& [key, cell] : dirty)
			m_SavingCells.insert(cell);
		m_DirtyCells.clear();
		m_DirtyVersion++;
		m_PersistentDirty = false;
		m_SavedSettings = m_Scene.GetSettings();
		m_Saving = true;
		outPlan = std::move(plan);
		return true;
	}

	void EditorWorld::CancelSave(EditorAssetManager& assets, const WorldSavePlan& plan)
	{
		for (const WorldSavePlan::Document& document : plan.Documents)
		{
			if (document.Kind == "cell")
				MarkCellDirty(document.Asset);
			if (document.New)
				assets.ReleaseNativeAssetReservation(document.Asset);
		}
		m_PersistentDirty = m_PersistentDirty || plan.PersistentDirty;
		m_SavingCells.clear();
		m_Saving = false;
	}

	std::optional<std::string> EditorWorld::FinishSave(EditorAssetManager& assets, const WorldSavePlan& plan, const DocumentSaveStatus& status,
		std::string* outError)
	{
		m_SavingCells.clear();
		RefreshCellTable();
		const Ref<WorldAsset> world = GetWorldAsset();
		WorldData data = world ? world->GetData() : WorldData();
		if (!data.Index)
			data.Index = WorldIndex();
		bool allWritten = true;
		for (size_t index = 0; index < plan.Documents.size(); index++)
		{
			const WorldSavePlan::Document& document = plan.Documents[index];
			// Each item that failed says why; the others are written and published.
			const bool written = index < status.Items.size() && status.Items[index].Error.empty();
			if (!written)
			{
				allWritten = false;
				// Not written: dirty again (a cell that failed is still loaded, its save having kept it).
				if (document.Kind == "cell")
					MarkCellDirty(document.Asset);
				else
					m_PersistentDirty = true;
				continue;
			}
			if (document.Kind != "cell")
				continue;
			// Its index entry, from what the save found in memory and the file now written.
			WorldCellSummary summary = document.Summary;
			summary.Stamp = fmt::format("{:016X}", assets.GetContentStamp(document.Asset));
			summary.ClosureBytes = WorldCellIO::GetClosureBytes(assets, document.Asset);
			std::vector<WorldCellSummary>& entries = data.Index->Cells;
			const auto entry = std::find_if(entries.begin(), entries.end(), [&](const WorldCellSummary& item) { return item.GetKey() == document.Key; });
			if (entry != entries.end())
				*entry = summary;
			else
				entries.push_back(summary);
		}
		// Cells made in memory whose documents are written (now or by an earlier save whose World document failed) join the
		// world's cells.
		for (const auto& [cell, key] : m_NewCells)
		{
			const bool listed = std::any_of(data.Cells.begin(), data.Cells.end(), [&](const WorldCell& item) { return item.Scene == cell; });
			if (!listed && assets.GetMetadata(cell))
				data.Cells.push_back(WorldCell { key.Layer, key.X, key.Z, cell });
		}
		if (allWritten)
			m_RequestedPins.clear(); // What undo steps asked for may go now
		if (!world || WorldAsset::ToDocument(data) == WorldAsset::ToDocument(world->GetData()))
		{
			m_Saving = false;
			return std::nullopt;
		}
		std::string error;
		if (!data.Validate(&error))
		{
			m_Saving = false;
			if (outError)
				*outError = fmt::format("The world's document would be invalid: {}", error);
			return std::nullopt;
		}
		return JsonUtils::Dump(WorldAsset::ToDocument(data), 1, '\t') + "\n";
	}

	void EditorWorld::FinishIndexSave()
	{
		m_Saving = false;
	}

	std::vector<WorldCellKey> EditorWorld::PrepareRuntimeCopy(Scene& copy) const
	{
		std::vector<Entity> partial;
		for (const SubSceneStatus& status : m_Scene.GetSubScenes())
		{
			if (!status.Scene.IsValid() || !m_CellKeys.contains(status.Scene))
				continue;
			std::vector<Entity> roots;
			for (const Entity& root : m_Scene.GetSubSceneRoots(status.ID))
			{
				if (const Entity copied = copy.GetEntityByUUID(root.GetUUID()))
					roots.push_back(copied);
			}
			if (status.State == SubSceneState::Loaded)
			{
				std::string error;
				if (copy.AdoptSubScene(status.Scene, roots, &error) == 0)
					ST_WARN("Play: the cell {} stays in the persistent scene: {}", DescribeCell(status.Scene), error);
			}
			else
			{
				partial.insert(partial.end(), roots.begin(), roots.end());
			}
		}
		copy.DestroyEntities(partial);

		// Cells a save still writes count too: their documents may not hold their edits yet.
		std::vector<WorldCellKey> dirty;
		for (const std::unordered_set<AssetHandle>* cells : { &m_DirtyCells, &m_SavingCells })
		{
			for (const AssetHandle cell : *cells)
			{
				if (const std::optional<WorldCellKey> key = FindCellKey(cell); key && !m_NewCells.contains(cell))
					dirty.push_back(*key);
			}
		}
		std::sort(dirty.begin(), dirty.end());
		return dirty;
	}

}
