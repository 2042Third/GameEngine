#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetResidency.h"
#include "Strata/Core/Base.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class AssetUploadContext;
	struct AssetManagerStats;

	// Work a higher module does for every asset manager inside its Update, such as the renderer's texture streamer, which
	// changes how much of a texture is resident: the manager calls it once per Update after the frame's arrivals were
	// finalized, within the same upload batch and with what is left of the frame's budgets, and asks it to free memory of a
	// pool over budget before it evicts whole assets. Each manager makes its own from the factories registered with
	// AssetManagerExtensionRegistry (AssetManagerBase::FindExtension finds them). Main thread only.
	class AssetManagerExtension
	{
	public:
		virtual ~AssetManagerExtension() = default;

		// Whether it keeps assets that loaded only partly (see AssetPrefixFunction) up to date with what they are needed
		// for, when the manager's finalizations upload through `upload` (null: none): the manager loads assets partly only
		// then (AssetManagerBase::SetPartialLoads).
		virtual bool RefinesPartialAssets(const AssetUploadContext* upload) const = 0;
		// Once per AssetManagerBase::Update, after the frame's arrivals were finalized: what it uploads goes into the open
		// batch of `context` and counts in its budgets (AssetFinalizeContext::UploadedBytes and UploadMs hold the frame's so
		// far; FirstOfFrame when the frame finalized nothing yet). Assets whose memory it changes are reported to the manager
		// (AssetManagerBase::UpdateMemoryUsage).
		virtual void OnUpdate(const AssetFinalizeContext& context) = 0;
		// Before the manager evicts assets for `pool` being over its budget: frees up to `bytes` of the pool from assets it
		// keeps partly resident (reporting each, AssetManagerBase::UpdateMemoryUsage), recording GPU work into the open batch
		// of `context`, and returns how much it freed. The manager then evicts whole assets for the rest.
		virtual uint64_t ReleaseMemory(AssetMemoryPool pool, uint64_t bytes, const AssetFinalizeContext& context) = 0;
		// Adds its counters to the manager's statistics (AssetManagerStats::Detail). Any thread.
		virtual void AddStats(AssetManagerStats& stats) const = 0;
	};

	// Makes the extension of a manager (called once, by the manager's constructor, on the main thread).
	using AssetManagerExtensionFactory = std::function<Scope<AssetManagerExtension>(AssetManagerBase& manager)>;

	// The factories of manager extensions, registered by the modules that provide them (Engine::RegisterBuiltinModules opens
	// the registry, like AssetLoaderRegistry): every manager constructed afterwards makes one extension per factory, in the
	// order they were registered.
	class AssetManagerExtensionRegistry
	{
	public:
		static void BeginRegistration();
		static void Register(AssetManagerExtensionFactory factory);
		// Makes the extensions for a new manager.
		static std::vector<Scope<AssetManagerExtension>> CreateExtensions(AssetManagerBase& manager);
	};

}
