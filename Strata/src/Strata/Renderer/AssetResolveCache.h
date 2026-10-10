#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/Base.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// What an AssetResolveCache did in the current frame.
	struct AssetResolveStats
	{
		uint32_t GetAssetCalls = 0;   // AssetManagerBase::GetAsset calls: at most one per distinct handle
		uint32_t Requested = 0;       // Distinct handles requested for use (Resolve)
		uint32_t Prefetched = 0;      // Distinct handles requested at Low priority without being used (Prefetch)
		uint32_t ResidentLookups = 0; // Distinct handles looked up without requesting a load (FindResident)
		uint32_t Pending = 0;         // Of the requested handles, those still loading
	};

	// Resolves asset handles for one renderer, once per handle and frame: the first request of a handle in a frame asks
	// the asset manager (one GetAsset, which also stamps the asset as used for the residency's least-recently-used order),
	// later ones in the same frame get the same answer. Nothing outlives the frame: EndFrame lets go of every reference
	// the frame took and of the manager, so a renderer that stops rendering (a capture renderer between captures, a hidden
	// view) keeps no asset from being evicted and no asset manager alive. BeginFrame also revalidates: it reports the
	// assets whose loaded objects changed since the previous frame (GetContentVersion, GetContentChanges), so data derived
	// from them can be refreshed. Main thread only.
	class AssetResolveCache
	{
	public:
		// Starts a frame with the active asset manager (null: none, every resolve fails), held until EndFrame. Appends the
		// assets whose objects changed since the previous frame to outChanged; outAllChanged when any may have (the manager
		// forgot, or another manager became active).
		void BeginFrame(const Ref<AssetManagerBase>& manager, std::vector<AssetHandle>& outChanged, bool& outAllChanged);
		// Ends the frame: releases the references it handed out and the manager. The GPU work recorded with the assets
		// keeps what it uses alive by itself (command lists reference their buffers, the bindless table its textures).
		void EndFrame();

		// The loaded asset, requested at `priority` (a load starts or is raised); null while it loads, for unknown and failed
		// handles and for another type.
		template<typename T>
		Ref<T> Resolve(AssetHandle handle, AssetPriority priority)
		{
			const Ref<Asset>& asset = ResolveAsset(handle, priority, false);
			return asset && asset->GetType() == T::GetStaticType() ? std::static_pointer_cast<T>(asset) : nullptr;
		}

		// The asset if it is loaded, without requesting a load (a loaded one is stamped as used, which keeps it resident).
		template<typename T>
		Ref<T> FindResident(AssetHandle handle)
		{
			const Ref<Asset>& asset = ResolveAsset(handle, AssetPriority::Low, true);
			return asset && asset->GetType() == T::GetStaticType() ? std::static_pointer_cast<T>(asset) : nullptr;
		}

		// Requests the load at Low priority without using the asset (prefetch); a loaded one is stamped as used.
		void Prefetch(AssetHandle handle);

		const AssetResolveStats& GetStats() const { return m_Stats; }
	private:
		enum class Lookup : uint8_t
		{
			None = 0,
			Resident, // FindResident: not requested
			Prefetch,
			Request
		};

		struct Entry
		{
			uint64_t Frame = 0;
			Ref<Asset> Asset; // Held until EndFrame
			Lookup Kind = Lookup::None;
		};

		// The entry's asset (null when there is none).
		const Ref<Asset>& ResolveAsset(AssetHandle handle, AssetPriority priority, bool residentOnly);
		Entry& GetEntry(AssetHandle handle);
	private:
		Ref<AssetManagerBase> m_Manager;               // The frame's manager, between BeginFrame and EndFrame
		std::weak_ptr<AssetManagerBase> m_LastManager; // The manager of the latest frame: tells whether the entries are its
		uint64_t m_ContentVersion = 0;
		uint64_t m_Frame = 0;
		// Entries stay where they are while the map lives (std::unordered_map never moves its elements), so m_Used can
		// point at them.
		std::unordered_map<AssetHandle, Entry> m_Entries;
		std::vector<Entry*> m_Used; // Entries holding references for the current frame
		AssetResolveStats m_Stats;
		static const Ref<Asset> s_None;
	};

}
