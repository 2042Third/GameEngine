#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Strata
{

	// Identifies an additive load of a scene into another (Scene::LoadSubScene, BeginAdditiveLoad), unique within the scene it
	// was loaded into; 0 names none.
	using SubSceneID = uint64_t;

	// Runtime only: on the top-level entities an additive load created, naming the sub-scene they came with, so that
	// unloading it removes them (with their descendants). Not registered with the ComponentRegistry (like
	// HierarchyComponent): never serialized, copied (Scene::Copy) or shown.
	struct SubSceneComponent
	{
		SubSceneID SubScene = 0;
		AssetHandle Source = UUID::Null(); // The scene asset (null for a prepared scene added directly)
	};

	enum class SubSceneState : uint8_t
	{
		Loading = 0,   // The scene asset and everything it uses load (SceneLoader), then its entities are compiled (job system)
		Instantiating, // Its entities are created, a slice in each update of the scene
		Loaded,        // Every entity of it exists
		Failed,        // The asset could not be loaded, or its entities could not be added (Error says why); nothing of it remains
		Unloading      // Being unloaded: its entities are inactive and go a slice per update (Scene::UnloadSubScene)
	};

	const char* SubSceneStateToString(SubSceneState state);

	// How a sub-scene loads (Scene::LoadSubScene).
	struct SubSceneOptions
	{
		// The priority the scene asset and what it uses are requested (and pinned) at while the sub-scene loads.
		AssetPriority Priority = AssetPriority::Normal;
		// The share of the stored bytes of the scene asset and its dependency closure that must be in before its entities are
		// created (SceneLoadOptions::RequiredProgress): all of it by default, so that they appear complete.
		float RequiredProgress = 1.0f;
	};

	// What became of a sub-scene so far (Scene::GetSubSceneStatus).
	struct SubSceneStatus
	{
		SubSceneID ID = 0;
		AssetHandle Scene = UUID::Null(); // The scene asset (null for a prepared scene added directly)
		SubSceneState State = SubSceneState::Loading;
		// The share done, in [0, 1]: while loading, half the share of the closure's stored bytes that is in; while
		// instantiating, a half plus half the share of its entities that exist.
		float Progress = 0.0f;
		size_t CreatedEntities = 0;
		// Entities not created because an ancestor the load created in an earlier update was destroyed meanwhile (they would
		// have gone with it).
		size_t SkippedEntities = 0;
		size_t TotalEntities = 0; // 0 until its entities are compiled
		uint32_t Slices = 0;      // Updates that created some of its entities
		std::string Error;
		size_t DestroyedEntities = 0; // While unloading: its entities destroyed so far
	};

}
