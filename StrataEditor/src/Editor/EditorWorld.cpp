#include "Editor/EditorWorld.h"

#include "Editor/WorldCellIO.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldComponents.h>

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
		m_CellKeys.clear();
		for (const WorldCell& cell : world->GetData().Cells)
			m_CellKeys.emplace(cell.Scene, cell.GetKey());
		ApplyVisibleLayers();
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
		m_DirtyCells.insert(cell);
	}

	void EditorWorld::MarkStates(const std::vector<EntityState>& states)
	{
		for (const EntityState& state : states)
		{
			if (!state.Parent.IsValid())
			{
				if (state.Cell.IsValid())
					MarkCellDirty(state.Cell);
				else
					m_PersistentDirty = true;
				continue;
			}
			// A descendant belongs to its top-level ancestor's document: the entity's now, else its parent's.
			Entity entity = m_Scene.GetEntityByUUID(state.ID);
			if (!entity)
				entity = m_Scene.GetEntityByUUID(state.Parent);
			const SubSceneStatus* status = entity ? m_Scene.GetSubSceneStatus(m_Scene.GetSubSceneOf(entity)) : nullptr;
			if (status && status->Scene.IsValid())
				MarkCellDirty(status->Scene);
			else
				m_PersistentDirty = true;
		}
	}

	void EditorWorld::OnApplied(const EditorAction& action, bool undone, bool)
	{
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
		for (const AssetHandle cell : m_DirtyCells)
		{
			if (const std::optional<WorldCellKey> key = FindCellKey(cell))
				wanted.insert(*key);
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

	void EditorWorld::Update(const glm::vec3& cameraPosition, const glm::vec3& cameraForward, const std::vector<UUID>& selection)
	{
		RefreshCellTable();
		// The cells of the selected entities stay while they are selected.
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
		SyncPins();

		WorldStreamingSource source;
		source.Position = cameraPosition;
		source.Forward = cameraForward;
		m_Sources.assign(1, source);
		m_Streamer.SetSources(m_Sources);
		m_Streamer.Update();
	}

	bool EditorWorld::Save(EditorAssetManager& assets, AssetHandle persistent, WorldSaveResult& outResult, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		outResult = {};
		RefreshCellTable();
		const Ref<WorldAsset> world = GetWorldAsset();
		if (!world)
			return fail("The world is still loading: save again in a moment");

		// The top-level entities by document, in hierarchy order: a cell's belong to its sub-scene, the rest to the persistent
		// scene (also those of other additive loads, as a plain scene's save keeps them).
		std::vector<Entity> persistentRoots;
		std::unordered_map<AssetHandle, std::vector<Entity>> cellRoots;
		for (const Entity entity : m_Scene.GetEntitiesInHierarchyOrder())
		{
			if (entity.GetComponent<RelationshipComponent>().Parent.IsValid())
				continue;
			const SubSceneStatus* status = m_Scene.GetSubSceneStatus(m_Scene.GetSubSceneOf(entity));
			if (status && status->Scene.IsValid() && m_CellKeys.contains(status->Scene))
				cellRoots[status->Scene].push_back(entity);
			else
				persistentRoots.push_back(entity);
		}

		// The persistent scene.
		const std::optional<AssetMetadata> persistentMetadata = assets.GetMetadata(persistent);
		if (!persistentMetadata)
			return fail("The persistent scene has no asset of the project");
		const nlohmann::json persistentDocument = WorldCellIO::SerializeDocument(m_Scene, m_Scene.GetName(), persistentRoots, true);
		outResult.PersistentDocument = JsonUtils::Dump(persistentDocument, 1, '\t') + "\n";
		if (m_PersistentDirty)
		{
			std::string error;
			const std::optional<size_t> bytes = WorldCellIO::WriteDocument(assets, persistent, AssetType::Scene, persistentDocument, false, &error);
			if (!bytes)
				return fail(fmt::format("The persistent scene cannot be saved: {}", error));
			outResult.Documents.push_back({ persistentMetadata->Path, "persistent", *bytes, persistentDocument["Scene"]["Entities"].size() });
			m_PersistentDirty = false;
		}
		else
		{
			std::vector<uint8_t> bytes;
			if (assets.ReadStoredData(persistent, bytes))
				outResult.PersistentDocument.assign(bytes.begin(), bytes.end());
		}

		// The dirty cells, in key order.
		std::vector<std::pair<WorldCellKey, AssetHandle>> dirty;
		for (const AssetHandle cell : m_DirtyCells)
		{
			if (const std::optional<WorldCellKey> key = FindCellKey(cell))
				dirty.emplace_back(*key, cell);
		}
		std::sort(dirty.begin(), dirty.end());
		WorldData data = world->GetData();
		if (!data.Index)
			data.Index = WorldIndex();
		for (const auto& [key, cell] : dirty)
		{
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(cell);
			if (!metadata)
				return fail(fmt::format("The cell {} has no asset of the project", DescribeKey(key)));
			const auto roots = cellRoots.find(cell);
			const nlohmann::json document = WorldCellIO::SerializeDocument(m_Scene, metadata->Name, roots != cellRoots.end() ? roots->second : std::vector<Entity>(), false);
			std::string error;
			const std::optional<size_t> bytes = WorldCellIO::WriteDocument(assets, cell, AssetType::Scene, document, false, &error);
			if (!bytes)
				return fail(fmt::format("The cell {} cannot be saved: {}", DescribeKey(key), error));
			outResult.Documents.push_back({ metadata->Path, "cell", *bytes, document["Scene"]["Entities"].size() });
			m_DirtyCells.erase(cell);

			// Its index entry, from what was just written.
			Scene saved("Cell");
			if (SceneSerializer::Deserialize(saved, document))
			{
				saved.UpdateWorldTransforms();
				WorldCellIO::CellSummary summary = WorldCellIO::SummarizeScene(saved, assets, cell);
				summary.Summary.Layer = key.Layer;
				summary.Summary.X = key.X;
				summary.Summary.Z = key.Z;
				summary.Summary.Stamp = fmt::format("{:016X}", assets.GetContentStamp(cell));
				std::vector<WorldCellSummary>& entries = data.Index->Cells;
				const auto entry = std::find_if(entries.begin(), entries.end(), [&](const WorldCellSummary& item) { return item.GetKey() == key; });
				if (entry != entries.end())
					*entry = summary.Summary;
				else
					entries.push_back(summary.Summary);
			}
		}
		// The World document, when the index changed.
		if (!dirty.empty() && WorldAsset::ToDocument(data) != WorldAsset::ToDocument(world->GetData()))
		{
			std::string error;
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(m_World);
			const std::optional<size_t> bytes = WorldCellIO::WriteDocument(assets, m_World, AssetType::World, WorldAsset::ToDocument(data), true, &error);
			if (!bytes)
				return fail(fmt::format("The world's document cannot be saved: {}", error));
			outResult.Documents.push_back({ metadata ? metadata->Path : m_World.ToString(), "world", *bytes, 0 });
		}
		// What undo steps asked for may go now.
		m_RequestedPins.clear();
		return true;
	}

}
