#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Physics/PhysicsTypes.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Asset;
	class AssetManagerBase;

	// The default PhysicsMeshProvider: the meshes of the active asset manager (AssetManager::GetActive).
	//
	// A mesh that is not loaded is requested (never blocking) and reported as unavailable until the asset manager has
	// finished loading it; built-in meshes (BuiltinAssets) are always loaded. The collision data of a mesh object is built
	// once (the full-detail level of every submesh) and returned for as long as the asset manager serves that object; a
	// reloaded mesh is a new object and gets new data. The version follows the asset manager's content version, and the
	// changed meshes come from its content changes (see AssetManagerBase::GetContentChanges), so physics re-reads only
	// the meshes that were loaded, reloaded or unloaded; the data of meshes that are no longer loaded is dropped then.
	// Main thread only.
	class AssetMeshProvider final : public PhysicsMeshProvider
	{
	public:
		Ref<const PhysicsMeshData> GetMeshData(AssetHandle mesh) override;
		Ref<const PhysicsMeshData> PeekMeshData(AssetHandle mesh) override;
		// A mesh the asset manager is loading.
		bool IsMeshLoading(AssetHandle mesh) override;
		uint64_t GetVersion() override;
		bool GetChangedMeshes(uint64_t version, std::vector<AssetHandle>& outMeshes) override;

		// Meshes whose collision data is held (for tests and statistics).
		size_t GetCachedMeshCount() const { return m_Entries.size(); }
	private:
		struct Entry
		{
			std::weak_ptr<const Asset> Source; // The mesh object the data was built from
			Ref<const PhysicsMeshData> Data;
		};

		// Follows the active asset manager: another one drops every entry and starts a new epoch of versions.
		void SyncWithManager(const Ref<AssetManagerBase>& manager);
		// Drops the data of meshes that are no longer loaded (those changed since sinceVersion when the manager can tell).
		void RemoveUnloadedMeshes(const Ref<AssetManagerBase>& manager, uint64_t sinceVersion);
	private:
		// Versions combine the epoch (which manager) with the manager's content version in the low bits.
		static constexpr uint32_t c_ContentVersionBits = 40;
		static constexpr uint64_t c_ContentVersionMask = (uint64_t(1) << c_ContentVersionBits) - 1;

		std::unordered_map<AssetHandle, Entry> m_Entries;
		std::weak_ptr<AssetManagerBase> m_Manager; // The asset manager the entries and versions refer to
		uint64_t m_Epoch = 0;
		uint64_t m_ContentVersion = 0;             // The manager's content version the entries were last checked at
		std::vector<AssetHandle> m_ChangedAssets;  // Scratch
	};

}
