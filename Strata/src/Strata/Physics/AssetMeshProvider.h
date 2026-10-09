#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Physics/PhysicsTypes.h"

#include <memory>
#include <unordered_map>

namespace Strata
{

	class Asset;
	class AssetManagerBase;

	// The default PhysicsMeshProvider: the meshes of the active asset manager (AssetManager::GetActive).
	//
	// A mesh that is not loaded is requested (never blocking) and reported as unavailable until the asset manager has
	// finished loading it; built-in meshes (BuiltinAssets) are always loaded. The collision data of a mesh object is built
	// once (the full-detail level of every submesh) and returned for as long as the asset manager serves that object; a
	// reloaded mesh is a new object and gets new data. The version follows the asset manager's content version, so that
	// physics re-reads meshes only after loads, reloads and unloads; the data of meshes that are no longer loaded is
	// dropped then. Main thread only.
	class AssetMeshProvider final : public PhysicsMeshProvider
	{
	public:
		Ref<const PhysicsMeshData> GetMeshData(AssetHandle mesh) override;
		uint64_t GetVersion() override;

		// Meshes whose collision data is held (for tests and statistics).
		size_t GetCachedMeshCount() const { return m_Entries.size(); }
	private:
		struct Entry
		{
			std::weak_ptr<const Asset> Source; // The mesh object the data was built from
			Ref<const PhysicsMeshData> Data;
		};

		// Drops the data of meshes that are no longer loaded, and everything when another asset manager became active.
		void RemoveStaleEntries(const Ref<AssetManagerBase>& manager);
	private:
		std::unordered_map<AssetHandle, Entry> m_Entries;
		std::weak_ptr<AssetManagerBase> m_EntriesManager; // The asset manager the entries came from

		std::weak_ptr<AssetManagerBase> m_VersionManager; // The asset manager the version follows
		uint64_t m_ManagerContentVersion = 0;
		uint64_t m_Version = 0;
	};

}
