#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Core/UUID.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/PrefabInstances.h>

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
	// the sync the new snapshot (SetBaseline), so they are not updated twice. A prefab that changed while the scene was
	// closed (applied from another scene, changed on disk) is caught up with once the scene is open, the first time the sync
	// looks at the prefab: instances whose recorded version of their part (PrefabInstances::MarkSynced) differs from the
	// prefab's and whose values still have the recorded hash take its values, in one undo step; modified ones are outdated
	// (IsOutdated). It also answers whether an instance is modified or outdated for the Hierarchy's marks, cached until the
	// edits or the prefab change. A scene the editor opens is caught up with at once (OnSceneOpened), before scene.open
	// answers, so that nothing that runs after the open sees instances that missed their prefab's changes.
	//
	// Comparing an instance serializes its entities, and finding the prefabs walks every link: work for an edit, not for every
	// frame of a run of edits (a gizmo drag records a step each frame). An edit after a pause is looked at at once; while
	// edits follow each other within c_SettleFrames frames, the marks and the watched prefabs stay as they were until the
	// edits pause.
	class PrefabSync
	{
	public:
		static constexpr uint64_t c_SettleFrames = 12;

		// Once per frame in edit mode while no scene is being opened (the edited scene is read-only then): looks at the source
		// revisions of the prefabs the scene's instances use (a few lookups; the set is found again only when links change or
		// edits pause) and updates instances of the ones that changed.
		void Update(EditorContext& context);
		// Once an opened scene replaced the edited one (EditorContext, in the frame it does): reads the template of every
		// prefab its instances use, one file each, and catches their instances up (CatchUp), so that commands in the frame of
		// the open (command scripts, pipelined requests) already see their values.
		void OnSceneOpened(EditorContext& context);
		// The snapshot instances of the prefab are compared with: its template when the sync last looked (read on first use).
		const nlohmann::json* GetBaseline(EditorContext& context, AssetHandle prefab);
		// After the editor wrote a prefab (prefab.apply): instances were updated with it, so it is the baseline now.
		void SetBaseline(EditorContext& context, AssetHandle prefab, nlohmann::json snapshot);
		// Whether an instance root differs from its prefab as stored now (cached per root until the edits or the prefab change;
		// during a run of edits, as it was before them).
		bool IsModified(EditorContext& context, Entity root);
		// Whether an instance root was made from (or last took) another version of its part of the prefab than the one stored
		// now without taking the newer one (PrefabInstances::IsOutdated): applying it would write older values back. Cached
		// like IsModified.
		bool IsOutdated(EditorContext& context, Entity root);
		// Forgets every baseline and mark (another scene or project).
		void Reset();

		// How many instances the sync updated after prefab changes (tests).
		uint64_t GetPropagationCount() const { return m_PropagationCount; }
		// How many instance roots the catch-up after a scene opened looked at (tests: once per instance and scene).
		uint64_t GetCatchUpCheckCount() const { return m_CatchUpCheckCount; }
		// How often an instance was compared for its mark, and the links walked for the watched prefabs (tests).
		uint64_t GetCompareCount() const { return m_CompareCount; }
		uint64_t GetWatchScanCount() const { return m_WatchScanCount; }
	private:
		struct Baseline
		{
			nlohmann::json Snapshot;
			uint64_t SourceRevision = 0;
			bool CaughtUp = false; // The scene's instances that missed changes of the prefab were looked at
			// The versions of the snapshot's parts, worked out once for all its instances (cleared with a new snapshot).
			PrefabInstances::PartVersionCache PartVersions;
		};
		struct ModifiedMark
		{
			uint64_t Changes = UINT64_MAX; // The undo history's change count and the baseline version it was worked out at
			uint64_t BaselineVersion = 0;
			bool Modified = false;
			bool Outdated = false;
		};

		// Updates the instances of a prefab whose file changed from the baseline to `snapshot`.
		void Propagate(EditorContext& context, AssetHandle prefab, const nlohmann::json& previous, const nlohmann::json& snapshot);
		// Updates the instances (`roots`, of the prefab) that missed a change of it while their scene was closed.
		void CatchUp(EditorContext& context, AssetHandle prefab, Baseline& baseline, const std::vector<Entity>& roots);
		// The baseline of a prefab, its template read on first use; null when it cannot be read.
		Baseline* FindBaseline(EditorContext& context, AssetHandle prefab);
		// Gives instance roots the snapshot's values, in one undo step.
		void UpdateInstances(EditorContext& context, AssetHandle prefab, const std::vector<Entity>& roots, const nlohmann::json& snapshot);
		void Watch(EditorContext& context);
		// Whether the history's change count (`changes`) is part of a run of edits that has not paused yet.
		bool AreEditsContinuing(uint64_t changes) const;
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
		uint64_t m_CatchUpCheckCount = 0;
		uint64_t m_CompareCount = 0;
		uint64_t m_WatchScanCount = 0;
		// Frames (Update calls), the change count Update saw last, and the frames of the last two changes it saw (0: none).
		uint64_t m_Frame = 0;
		uint64_t m_SeenChanges = UINT64_MAX;
		uint64_t m_LastChangeFrame = 0;
		uint64_t m_PreviousChangeFrame = 0;
	};

}
