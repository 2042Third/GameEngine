#include "Editor/PrefabSync.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneEdit.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace Strata
{

	namespace
	{

		// How close an instance root's Scale must be to a model's import scale to be taken for it (relative).
		constexpr float c_ImportScaleTolerance = 1.0e-5f;

		// The Scale of a template root in a snapshot (its Transform's), or nullopt when the snapshot lacks it.
		std::optional<glm::vec3> FindTemplateRootScale(const nlohmann::json& snapshot, UUID templateRoot)
		{
			const nlohmann::json* entities = JsonUtils::Find(snapshot, "Entities");
			if (!entities || !entities->is_array())
				return std::nullopt;
			for (const nlohmann::json& entity : *entities)
			{
				const nlohmann::json* id = JsonUtils::Find(entity, "ID");
				if (!id || UUIDFromJson(*id) != std::optional<UUID>(templateRoot))
					continue;
				const nlohmann::json* components = JsonUtils::Find(entity, "Components");
				const nlohmann::json* transform = components ? JsonUtils::Find(*components, "Transform") : nullptr;
				const nlohmann::json* scale = transform ? JsonUtils::Find(*transform, "Scale") : nullptr;
				if (!scale || !scale->is_array() || scale->size() != 3 || !(*scale)[0].is_number() || !(*scale)[1].is_number() || !(*scale)[2].is_number())
					return glm::vec3(1.0f); // The Transform's default
				return glm::vec3((*scale)[0].get<float>(), (*scale)[1].get<float>(), (*scale)[2].get<float>());
			}
			return std::nullopt;
		}

		bool IsNear(float value, float expected)
		{
			return std::abs(value - expected) <= c_ImportScaleTolerance * std::abs(expected);
		}

	}

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
		const uint64_t changes = context.GetUndoStack().GetVersion();
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
		const uint64_t changes = context.GetUndoStack().GetVersion();
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
				found = m_Baselines.emplace(prefab, Baseline { snapshot ? std::move(*snapshot) : nlohmann::json(), revision }).first;
			}
			// Once per prefab and scene, with the first look at it: instances that missed a change of it while their scene was
			// closed.
			if (!found->second.CaughtUp)
			{
				found->second.CaughtUp = true;
				if (!found->second.Snapshot.is_null())
					CatchUp(context, prefab, found->second, PrefabInstances::FindInstanceRoots(*scene, prefab));
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
			found->second.PartVersions.Clear();
			m_BaselineVersion++;
			if (!previous.is_null())
				Propagate(context, prefab, previous, *snapshot);
		}
	}

	void PrefabSync::OnSceneOpened(EditorContext& context)
	{
		Reset();
		EditorAssetManager* assets = context.GetAssetManager();
		const Ref<Scene>& scene = context.GetEditScene();
		m_Scene = scene;
		m_Assets = assets;
		if (!assets || !scene)
			return;
		Watch(context);
		// Every prefab at once rather than one per frame (Update): bounded by the prefabs the scene uses, in the frame that
		// swaps the scene in anyway. The instance roots of all of them are found in one pass over the links.
		const std::unordered_map<AssetHandle, std::vector<UUID>> rootsByPrefab = PrefabInstances::FindInstanceRootsByPrefab(*scene);
		for (const AssetHandle prefab : m_Watched)
		{
			const uint64_t revision = assets->GetSourceRevisions(prefab).Current;
			if (revision == 0)
				continue; // No asset of the project (deleted, a built-in)
			std::optional<nlohmann::json> snapshot = PrefabSyncUtils::ReadTemplate(*assets, prefab);
			Baseline& baseline = m_Baselines.insert_or_assign(prefab, Baseline { snapshot ? std::move(*snapshot) : nlohmann::json(), revision }).first->second;
			baseline.CaughtUp = true;
			const auto found = rootsByPrefab.find(prefab);
			if (baseline.Snapshot.is_null() || found == rootsByPrefab.end())
				continue;
			// Resolved now: updating the instances of a prefab before may have removed some (instances nested in its instances).
			std::vector<Entity> roots;
			roots.reserve(found->second.size());
			for (const UUID id : found->second)
			{
				const Entity root = scene->GetEntityByUUID(id);
				if (root && PrefabInstances::IsInstanceRoot(root) && PrefabInstances::GetPrefab(root) == prefab)
					roots.push_back(root);
			}
			CatchUp(context, prefab, baseline, roots);
		}
	}

	void PrefabSync::Propagate(EditorContext& context, AssetHandle prefab, const nlohmann::json& previous, const nlohmann::json& snapshot)
	{
		Scene& scene = *context.GetEditScene();
		// A model whose root Scale changed from a to b (an import made before the import scale was baked into the content, or
		// a document someone wrote): every instance root's Scale is multiplied by b / a per axis, so that instances keep the
		// size the model gives them now instead of taking the scale twice.
		const bool model = context.GetAssetManager()->GetAssetType(prefab) == AssetType::Model;
		std::vector<Entity> unchanged;
		std::vector<RootRescale> rescaled;
		for (const Entity root : PrefabInstances::FindInstanceRoots(scene, prefab))
		{
			if (PrefabInstances::Compare(scene, root, previous).IsEmpty())
				unchanged.push_back(root);
			if (!model)
				continue;
			const UUID templateRoot = root.GetComponent<PrefabInstanceComponent>().PrefabEntityID;
			const std::optional<glm::vec3> before = FindTemplateRootScale(previous, templateRoot);
			const std::optional<glm::vec3> after = FindTemplateRootScale(snapshot, templateRoot);
			if (!before || !after || *before == *after || before->x == 0.0f || before->y == 0.0f || before->z == 0.0f)
				continue;
			rescaled.push_back(RootRescale { root, root.GetComponent<TransformComponent>().Scale * (*after / *before) });
		}
		UpdateInstances(context, prefab, unchanged, snapshot, rescaled);
	}

	void PrefabSync::CatchUp(EditorContext& context, AssetHandle prefab, Baseline& baseline, const std::vector<Entity>& roots)
	{
		// An instance whose part of the prefab is another version than the one it last took (MarkSynced), and whose values
		// are still the ones it had then, missed the prefab's change; one modified since keeps its values (IsOutdated says so).
		// Each part's version is worked out once, however many instances there are; an instance's values are hashed only when
		// its version is out of date.
		Scene& scene = *context.GetEditScene();
		// Without the previous snapshot, a model instance placed before the import scale was baked into the content is known by
		// its root: outdated, with the model's import Scale setting as its Scale while the new template root has Scale 1. It
		// gets Scale 1, or it would take the scale twice.
		EditorAssetManager& assets = *context.GetAssetManager();
		std::optional<float> importScale;
		if (assets.GetAssetType(prefab) == AssetType::Model)
		{
			const nlohmann::json settings = assets.GetImportSettings(prefab);
			const nlohmann::json* scale = JsonUtils::Find(settings, "Scale");
			if (scale && scale->is_number() && scale->get<float>() > 0.0f && scale->get<float>() != 1.0f)
				importScale = scale->get<float>();
		}
		std::vector<Entity> missed;
		std::vector<RootRescale> rescaled;
		for (const Entity root : roots)
		{
			m_CatchUpCheckCount++;
			if (!PrefabInstances::IsOutdated(root, baseline.Snapshot, baseline.PartVersions))
				continue;
			if (PrefabInstances::IsUnchangedSinceSync(scene, root))
				missed.push_back(root);
			if (!importScale)
				continue;
			const glm::vec3 scale = root.GetComponent<TransformComponent>().Scale;
			const std::optional<glm::vec3> templateScale = FindTemplateRootScale(baseline.Snapshot, root.GetComponent<PrefabInstanceComponent>().PrefabEntityID);
			if (templateScale == std::optional<glm::vec3>(glm::vec3(1.0f)) && IsNear(scale.x, *importScale) && IsNear(scale.y, *importScale) && IsNear(scale.z, *importScale))
				rescaled.push_back(RootRescale { root, glm::vec3(1.0f) });
		}
		if (!rescaled.empty())
		{
			const std::optional<AssetMetadata> metadata = assets.GetMetadata(prefab);
			ST_INFO("{} instance{} of {} kept {} size: the import scale is now part of the model", rescaled.size(), rescaled.size() == 1 ? "" : "s",
				metadata ? metadata->Name : prefab.ToString(), rescaled.size() == 1 ? "its" : "their");
		}
		UpdateInstances(context, prefab, missed, baseline.Snapshot, rescaled);
	}

	void PrefabSync::UpdateInstances(EditorContext& context, AssetHandle prefab, const std::vector<Entity>& roots, const nlohmann::json& snapshot,
		const std::vector<RootRescale>& rescaled)
	{
		if (roots.empty() && rescaled.empty())
			return;
		Scene& scene = *context.GetEditScene();
		const std::optional<AssetMetadata> metadata = context.GetAssetManager()->GetMetadata(prefab);
		SceneEditTransaction transaction(scene, fmt::format("Update Instances of {}", metadata ? metadata->Name : prefab.ToString()), {});
		for (const Entity root : roots)
			transaction.TrackSubtree(root.GetUUID());
		for (const RootRescale& rescale : rescaled)
			transaction.Track(rescale.Root.GetUUID());
		// The roots' Scale first: Replace keeps a root's transform.
		for (const RootRescale& rescale : rescaled)
		{
			Entity root = scene.GetEntityByUUID(rescale.Root.GetUUID());
			if (!root)
				continue;
			root.GetComponent<TransformComponent>().Scale = rescale.Scale;
			root.MarkModified<TransformComponent>();
		}
		for (const Entity root : roots)
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

	PrefabSync::Baseline* PrefabSync::FindBaseline(EditorContext& context, AssetHandle prefab)
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
		return found->second.Snapshot.is_null() ? nullptr : &found->second;
	}

	const nlohmann::json* PrefabSync::GetBaseline(EditorContext& context, AssetHandle prefab)
	{
		const Baseline* baseline = FindBaseline(context, prefab);
		return baseline ? &baseline->Snapshot : nullptr;
	}

	void PrefabSync::SetBaseline(EditorContext& context, AssetHandle prefab, nlohmann::json snapshot)
	{
		EditorAssetManager* assets = context.GetAssetManager();
		const auto found = m_Baselines.find(prefab);
		const bool caughtUp = found != m_Baselines.end() && found->second.CaughtUp;
		m_Baselines[prefab] = Baseline { std::move(snapshot), assets ? assets->GetSourceRevisions(prefab).Current : 0, caughtUp };
		m_BaselineVersion++;
	}

	bool PrefabSync::IsModified(EditorContext& context, Entity root)
	{
		if (!root || !PrefabInstances::IsInstanceRoot(root))
			return false;
		// Edits and baselines both change what an instance is compared with. A new baseline renews the mark at once; edits do
		// once they pause (a mark worked out before a run of edits stays during it).
		const uint64_t changes = context.GetUndoStack().GetVersion();
		auto [it, added] = m_Marks.try_emplace(root.GetUUID());
		ModifiedMark& mark = it->second;
		if (!added && mark.BaselineVersion == m_BaselineVersion && (mark.Changes == changes || AreEditsContinuing(changes)))
			return mark.Modified;
		Baseline* baseline = FindBaseline(context, PrefabInstances::GetPrefab(root));
		// The instance's override list says so; only instances without one (scenes from before, unmodified ones) are compared.
		if (!root.GetComponent<PrefabInstanceComponent>().Overrides.empty())
		{
			mark.Modified = true;
		}
		else
		{
			m_CompareCount++;
			mark.Modified = baseline && !PrefabInstances::Compare(*root.GetScene(), root, baseline->Snapshot).IsEmpty();
		}
		mark.Outdated = baseline && PrefabInstances::IsOutdated(root, baseline->Snapshot, baseline->PartVersions);
		mark.Changes = changes;
		mark.BaselineVersion = m_BaselineVersion;
		return mark.Modified;
	}

	bool PrefabSync::IsOutdated(EditorContext& context, Entity root)
	{
		if (!root || !PrefabInstances::IsInstanceRoot(root))
			return false;
		IsModified(context, root);
		return m_Marks.at(root.GetUUID()).Outdated;
	}

}
