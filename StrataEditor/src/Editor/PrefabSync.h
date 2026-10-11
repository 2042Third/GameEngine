#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/UUID.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class EditorAssetManager;
	class EditorContext;
	class Scene;

	namespace PrefabSyncUtils
	{

		// The entity snapshot ({"Entities": [...]}) of a prefab or model as stored now: the prefab's file, a model's imported
		// document. Fails for other assets and unreadable data.
		std::optional<nlohmann::json> ReadTemplate(EditorAssetManager& assets, AssetHandle prefab, std::string* outError = nullptr);

	}

	// Keeps the prefab instances of the edited scene in step with their prefabs (EditorContext::Update, in edit mode): when a
	// prefab's file changes (a reimport, a change on disk, an undo of prefab.apply goes with its own instances), instances
	// whose values equalled the prefab's before (PrefabInstances::Compare, the root's transform and name left out) take its
	// new values, in one undo step; modified instances keep theirs. prefab.apply updates the other instances itself and tells
	// the sync the new snapshot (SetBaseline), so they are not updated twice. It also answers whether an instance is
	// modified for the Hierarchy's marks, cached until the edits or the prefab change.
	class PrefabSync
	{
	public:
		// Once per frame in edit mode: looks at the source revisions of the prefabs the scene's instances use (a few lookups;
		// the set is found again only when links, edits or the scene change) and updates instances of the ones that changed.
		void Update(EditorContext& context);
		// The snapshot instances of the prefab are compared with: its template when the sync last looked (read on first use).
		const nlohmann::json* GetBaseline(EditorContext& context, AssetHandle prefab);
		// After the editor wrote a prefab (prefab.apply): instances were updated with it, so it is the baseline now.
		void SetBaseline(EditorContext& context, AssetHandle prefab, nlohmann::json snapshot);
		// Whether an instance root differs from its prefab as stored now (cached per root until the edits or the prefab change).
		bool IsModified(EditorContext& context, Entity root);
		// Forgets every baseline and mark (another scene or project).
		void Reset();

		// How many instances the sync updated after prefab changes (tests).
		uint64_t GetPropagationCount() const { return m_PropagationCount; }
	private:
		struct Baseline
		{
			nlohmann::json Snapshot;
			uint64_t SourceRevision = 0;
		};
		struct ModifiedMark
		{
			uint64_t Stamp = 0;
			bool Modified = false;
		};

		// Updates the instances of a prefab whose file changed from the baseline to `snapshot`.
		void Propagate(EditorContext& context, AssetHandle prefab, const nlohmann::json& previous, const nlohmann::json& snapshot);
		void Watch(EditorContext& context);
		uint64_t GetStamp(EditorContext& context) const;
	private:
		std::weak_ptr<Scene> m_Scene;
		const EditorAssetManager* m_Assets = nullptr;
		std::unordered_map<AssetHandle, Baseline> m_Baselines;
		std::vector<AssetHandle> m_Watched; // The prefabs the scene's instances use
		uint64_t m_WatchedChanges = UINT64_MAX; // The undo history's change count and the link count when they were found
		size_t m_WatchedLinks = SIZE_MAX;
		uint64_t m_BaselineVersion = 0;     // Changes with every baseline
		std::unordered_map<UUID, ModifiedMark> m_Marks;
		uint64_t m_PropagationCount = 0;
	};

}
