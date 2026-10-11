#include "Editor/PrefabSync.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <set>
#include <utility>

namespace Strata
{

	namespace PrefabSyncUtils
	{

		std::optional<nlohmann::json> ReadTemplate(EditorAssetManager& assets, AssetHandle prefab, std::string* outError)
		{
			auto fail = [outError](std::string message) -> std::optional<nlohmann::json>
			{
				if (outError)
					*outError = std::move(message);
				return std::nullopt;
			};

			const AssetType type = assets.GetAssetType(prefab);
			const char* key = type == AssetType::Prefab ? "Prefab" : type == AssetType::Model ? "Model" : nullptr;
			if (!key)
				return fail(fmt::format("{} is no prefab or model of the project", prefab.ToString()));
			std::vector<uint8_t> data;
			std::string error;
			if (!assets.ReadStoredData(prefab, data, &error))
				return fail(error);
			std::optional<nlohmann::json> document = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), &error);
			const nlohmann::json* snapshot = document ? JsonUtils::Find(*document, key) : nullptr;
			const nlohmann::json* entities = snapshot ? JsonUtils::Find(*snapshot, "Entities") : nullptr;
			if (!entities || !entities->is_array())
				return fail(fmt::format("The {} cannot be read: {}", AssetTypeToString(type), error.empty() ? std::string("it has no entity list") : error));
			return *snapshot;
		}

	}

	void PrefabSync::Reset()
	{
		m_Baselines.clear();
		m_Watched.clear();
		m_WatchedChanges = UINT64_MAX;
		m_WatchedLinks = SIZE_MAX;
		m_Marks.clear();
		m_BaselineVersion++;
	}

	bool PrefabSync::AreEditsContinuing(uint64_t changes) const
	{
		// A change Update has not seen yet (made in this frame) continues a run when the last change it saw was recent.
		if (changes != m_SeenChanges)
			return m_LastChangeFrame != 0 && m_Frame - m_LastChangeFrame < c_SettleFrames;
		return m_PreviousChangeFrame != 0 && m_LastChangeFrame - m_PreviousChangeFrame < c_SettleFrames && m_Frame - m_LastChangeFrame < c_SettleFrames;
	}

	void PrefabSync::Watch(EditorContext& context)
	{
		Scene& scene = *context.GetEditScene();
		const auto* links = std::as_const(scene.GetRegistry()).storage<PrefabInstanceComponent>();
		const size_t linkCount = links ? links->size() : 0;
		const uint64_t changes = context.GetUndoStack().GetChangeCount();
		// Links that come or go are found at once; edits that may have replaced links once they pause.
		if (linkCount == m_WatchedLinks && (changes == m_WatchedChanges || AreEditsContinuing(changes)))
			return;
		m_WatchScanCount++;
		m_WatchedChanges = changes;
		m_WatchedLinks = linkCount;
		std::set<AssetHandle> prefabs;
		if (links)
		{
			for (const PrefabInstanceComponent& link : *links)
				prefabs.insert(link.Prefab);
		}
		m_Watched.assign(prefabs.begin(), prefabs.end());
	}

	void PrefabSync::Update(EditorContext& context)
	{
		// The frames of the history's changes, for AreEditsContinuing (the first count seen is no change).
		m_Frame++;
		const uint64_t changes = context.GetUndoStack().GetChangeCount();
		if (changes != m_SeenChanges)
		{
			if (m_SeenChanges != UINT64_MAX)
			{
				m_PreviousChangeFrame = m_LastChangeFrame;
				m_LastChangeFrame = m_Frame;
			}
			m_SeenChanges = changes;
		}

		EditorAssetManager* assets = context.GetAssetManager();
		const Ref<Scene>& scene = context.GetEditScene();
		if (!assets || !scene)
		{
			if (m_Assets)
				Reset();
			m_Assets = nullptr;
			return;
		}
		if (m_Scene.lock() != scene || m_Assets != assets)
		{
			Reset();
			m_Scene = scene;
			m_Assets = assets;
		}
		Watch(context);

		bool readOne = false;
		for (const AssetHandle prefab : m_Watched)
		{
			const uint64_t revision = assets->GetSourceRevisions(prefab).Current;
			if (revision == 0)
				continue; // No asset of the project (deleted, a built-in)
			auto found = m_Baselines.find(prefab);
			if (found == m_Baselines.end())
			{
				// The template instances are compared with when the prefab changes, read before it does (one per frame).
				if (readOne)
					continue;
				readOne = true;
				std::optional<nlohmann::json> snapshot = PrefabSyncUtils::ReadTemplate(*assets, prefab);
				m_Baselines.emplace(prefab, Baseline { snapshot ? std::move(*snapshot) : nlohmann::json(), revision });
				continue;
			}
			if (found->second.SourceRevision == revision)
				continue;
			found->second.SourceRevision = revision;
			std::string error;
			std::optional<nlohmann::json> snapshot = PrefabSyncUtils::ReadTemplate(*assets, prefab, &error);
			if (!snapshot)
			{
				ST_WARN("The prefab {} changed but cannot be read, so its instances keep their values: {}", prefab.ToString(), error);
				continue;
			}
			// The editor's own writes (prefab.apply and its undo) set the baseline before the file's revision follows.
			if (*snapshot == found->second.Snapshot)
				continue;
			nlohmann::json previous = std::move(found->second.Snapshot);
			found->second.Snapshot = *snapshot;
			m_BaselineVersion++;
			if (!previous.is_null())
				Propagate(context, prefab, previous, *snapshot);
		}
	}

	void PrefabSync::Propagate(EditorContext& context, AssetHandle prefab, const nlohmann::json& previous, const nlohmann::json& snapshot)
	{
		Scene& scene = *context.GetEditScene();
		std::vector<Entity> unchanged;
		for (const Entity root : PrefabInstances::FindInstanceRoots(scene, prefab))
		{
			if (PrefabInstances::Compare(scene, root, previous).IsEmpty())
				unchanged.push_back(root);
		}
		if (unchanged.empty())
			return;
		const std::optional<AssetMetadata> metadata = context.GetAssetManager()->GetMetadata(prefab);
		SceneEditTransaction transaction(scene, fmt::format("Update Instances of {}", metadata ? metadata->Name : prefab.ToString()), {});
		for (const Entity root : unchanged)
			transaction.TrackSubtree(root.GetUUID());
		for (const Entity root : unchanged)
		{
			const UUID id = root.GetUUID();
			std::vector<UUID> created;
			std::string error;
			const Entity current = scene.GetEntityByUUID(id);
			if (!current || !PrefabInstances::Replace(scene, current, snapshot, &created, &error))
			{
				ST_WARN("An instance of {} could not take its new values: {}", metadata ? metadata->Path : prefab.ToString(), error);
				continue;
			}
			for (const UUID entity : created)
				transaction.TrackCreated(entity);
			m_PropagationCount++;
		}
		context.CommitEdit(transaction);
		context.PruneSelection();
	}

	const nlohmann::json* PrefabSync::GetBaseline(EditorContext& context, AssetHandle prefab)
	{
		auto found = m_Baselines.find(prefab);
		if (found == m_Baselines.end())
		{
			EditorAssetManager* assets = context.GetAssetManager();
			if (!assets)
				return nullptr;
			std::optional<nlohmann::json> snapshot = PrefabSyncUtils::ReadTemplate(*assets, prefab);
			if (!snapshot)
				return nullptr;
			found = m_Baselines.emplace(prefab, Baseline { std::move(*snapshot), assets->GetSourceRevisions(prefab).Current }).first;
		}
		return found->second.Snapshot.is_null() ? nullptr : &found->second.Snapshot;
	}

	void PrefabSync::SetBaseline(EditorContext& context, AssetHandle prefab, nlohmann::json snapshot)
	{
		EditorAssetManager* assets = context.GetAssetManager();
		m_Baselines[prefab] = Baseline { std::move(snapshot), assets ? assets->GetSourceRevisions(prefab).Current : 0 };
		m_BaselineVersion++;
	}

	bool PrefabSync::IsModified(EditorContext& context, Entity root)
	{
		if (!root || !PrefabInstances::IsInstanceRoot(root))
			return false;
		// Edits and baselines both change what an instance is compared with. A new baseline renews the mark at once; edits do
		// once they pause (a mark worked out before a run of edits stays during it).
		const uint64_t changes = context.GetUndoStack().GetChangeCount();
		auto [it, added] = m_Marks.try_emplace(root.GetUUID());
		ModifiedMark& mark = it->second;
		if (!added && mark.BaselineVersion == m_BaselineVersion && (mark.Changes == changes || AreEditsContinuing(changes)))
			return mark.Modified;
		m_CompareCount++;
		const nlohmann::json* baseline = GetBaseline(context, PrefabInstances::GetPrefab(root));
		mark.Modified = baseline && !PrefabInstances::Compare(*root.GetScene(), root, *baseline).IsEmpty();
		mark.Changes = changes;
		mark.BaselineVersion = m_BaselineVersion;
		return mark.Modified;
	}

}
