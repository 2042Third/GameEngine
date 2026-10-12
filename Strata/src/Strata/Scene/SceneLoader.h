#pragma once

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Base.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Strata
{

	class Scene;

	enum class SceneLoadState : uint8_t
	{
		LoadingScene = 0,    // The scene asset is read and its scene prepared (on the loading worker)
		LoadingDependencies, // The scene is prepared; what it uses is still loading
		Ready,               // The scene and enough of what it uses are loaded (SceneLoadOptions::RequiredProgress)
		Failed,              // The scene asset could not be loaded (GetError)
		Cancelled            // Cancel was called
	};

	const char* SceneLoadStateToString(SceneLoadState state);

	struct SceneLoadOptions
	{
		// The priority the scene and what it uses are requested (and pinned) at.
		AssetPriority Priority = AssetPriority::High;
		// The share of the stored bytes of the scene and what it uses (the part of its dependency closure the load takes, see
		// SceneLoadRequest) that must have loaded before the load is Ready, in [0, 1]: 1 waits for all of it (a game that
		// switches only once its next level can be drawn complete), 0 only for the scene itself.
		float RequiredProgress = 1.0f;
		// Read the scene asset's stored form again even when it is loaded (an editor opens the file as it is now).
		bool Reload = false;
	};

	// One asynchronous scene load (SceneLoader::LoadAsync): the scene asset is read into a prepared scene on the loading
	// worker (SceneAsset) while what it uses loads alongside, pinned at the load's priority so that nothing of it is evicted
	// (AssetManagerBase::RequestLoadGroup). What it uses is its dependency closure without the other scenes it names
	// (AssetClosureScope::Load: a script field holding the next level does not load that level), as far as the residency
	// budgets have room: a pool's members load with the scene when all of them fit in the room the pool's budget has beside
	// what is resident when the load starts (estimated from their stored sizes; members loaded already always count).
	// Otherwise the scene requests that pool's members itself once it runs, what it draws first, and so it does members
	// that depend directly on one of them (requesting those would prefetch them): the load cannot know which part the scene
	// draws, and any other part would take the room from it (GetSkippedAssetCount). So a load never pushes a pool past its
	// budget. Advance it once per frame with Update; once it is Ready, TakeScene hands the scene over. The pins live as long
	// as the request (or until Cancel): owners drop it once the scene they took has started, so that its assets are what
	// the scene requests from then on. Main thread only; movable, not copyable.
	class SceneLoadRequest
	{
	public:
		SceneLoadRequest() = default;
		SceneLoadRequest(SceneLoadRequest&&) noexcept = default;
		SceneLoadRequest& operator=(SceneLoadRequest&&) noexcept = default;
		SceneLoadRequest(const SceneLoadRequest&) = delete;
		SceneLoadRequest& operator=(const SceneLoadRequest&) = delete;
		~SceneLoadRequest();

		// Whether the request names a load (default-constructed ones do not).
		bool IsValid() const { return m_Scene.IsValid(); }
		AssetHandle GetSceneAsset() const { return m_Scene; }
		SceneLoadState GetState() const { return m_State; }
		bool IsFinished() const { return m_State == SceneLoadState::Ready || m_State == SceneLoadState::Failed || m_State == SceneLoadState::Cancelled; }
		const std::string& GetError() const { return m_Error; }
		// The share of the stored bytes of the scene and its dependency closure that finished loading, in [0, 1]; it never
		// decreases (1 once Ready). While the scene's document is read, the share of the read done so far
		// (SceneReadProgress) counts for its part of the bytes, so that the progress moves on during the seconds a large
		// scene takes to read.
		float GetProgress() const;
		// The stored bytes the load waits for (the scene asset included) and how many assets that is.
		uint64_t GetTotalBytes() const { return m_TotalBytes; }
		size_t GetAssetCount() const { return m_Group.GetAssets().size(); }
		// Those assets (the scene asset first): what its owner keeps when it trims after the switch (ScheduleTrim).
		const std::vector<AssetHandle>& GetAssets() const { return m_Group.GetAssets(); }
		// The assets of the closure the load leaves to the scene, because the budgets had no room for their pools' share of it
		// when it started (or they depend on such an asset).
		size_t GetSkippedAssetCount() const { return m_SkippedAssets; }

		// Advances the load: takes the prepared scene once its asset is loaded (the spent asset is unloaded, so a later load
		// reads the file again) and becomes Ready once the closure's progress reaches the options' requirement. A dependency
		// that fails to load, or is unregistered meanwhile, counts as finished (the scene shows a fallback for it); a scene
		// asset that fails, or is unregistered before it was read, fails the load. Main thread, once per frame (after the
		// asset manager's Update).
		void Update();
		// The prepared scene, once Ready: the first call takes it (the request keeps its pins), later calls return null.
		Ref<Scene> TakeScene();
		// Abandons the load: releases every pin and abandons the loads the request started that have not finished
		// (AssetGroupRequest::Cancel), and drops a scene prepared already. Nothing changes for a finished request but its
		// pins, which go.
		void Cancel();
	private:
		friend class SceneLoader;

		void Fail(std::string error);
	private:
		std::weak_ptr<AssetManagerBase> m_Manager;
		AssetHandle m_Scene = UUID::Null();
		SceneLoadOptions m_Options;
		SceneLoadState m_State = SceneLoadState::Failed;
		std::string m_Error;
		AssetGroupRequest m_Group;
		uint64_t m_TotalBytes = 0;
		size_t m_SkippedAssets = 0;
		// The scene asset's part of the closure as the group weighs it (AssetGroupRequest: stored sizes of at least 1).
		float m_SceneShare = 0.0f;
		mutable float m_ReportedProgress = 0.0f;
		Ref<Scene> m_Prepared;
		bool m_Taken = false;
	};

	// Starts asynchronous scene loads from the active asset manager (see SceneLoadRequest). Main thread only.
	class SceneLoader
	{
	public:
		// A request for the scene asset `scene`; it fails right away (Failed, with the reason) without an active asset
		// manager or for a handle that is not a scene asset.
		static SceneLoadRequest LoadAsync(AssetHandle scene, const SceneLoadOptions& options = {});
	};

}
