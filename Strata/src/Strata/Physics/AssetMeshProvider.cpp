#include "stpch.h"
#include "Strata/Physics/AssetMeshProvider.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/MeshGeometry.h"

namespace Strata
{

	namespace
	{

		// Whether a weak and a shared pointer refer to the same object (also when the weak one expired since: owner identity
		// cannot be confused with a new object at the same address).
		bool IsSameOwner(const std::weak_ptr<AssetManagerBase>& a, const Ref<AssetManagerBase>& b)
		{
			return !a.owner_before(b) && !b.owner_before(a);
		}

	}

	Ref<const PhysicsMeshData> AssetMeshProvider::GetMeshData(AssetHandle mesh)
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		SyncWithManager(manager);
		if (!manager || !mesh.IsValid())
			return nullptr;

		// Requests the load if the mesh is not loaded. While a mesh is reloaded, the previous object is still served.
		const Ref<Asset> asset = manager->GetAsset(mesh);
		if (!asset)
			return nullptr;

		auto it = m_Entries.find(mesh);
		if (it != m_Entries.end() && it->second.Source.lock() == asset)
			return it->second.Data;

		// First use, or a new object (reloaded): new data, so that physics rebuilds the colliders using it. The module that
		// owns the asset's type reads its triangles (the renderer's meshes); other assets have none.
		MeshGeometry geometry;
		if (!MeshGeometryRegistry::Read(*asset, geometry))
			return nullptr;
		Ref<PhysicsMeshData> data = CreateRef<PhysicsMeshData>();
		data->Positions = std::move(geometry.Positions);
		data->Indices = std::move(geometry.Indices);
		m_Entries[mesh] = Entry { asset, data };
		return data;
	}

	Ref<const PhysicsMeshData> AssetMeshProvider::PeekMeshData(AssetHandle mesh)
	{
		// An unloaded mesh is not requested: GetAsset only starts loading assets in that state.
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (!manager || !mesh.IsValid() || manager->GetAssetState(mesh) == AssetState::Unloaded)
			return nullptr;
		return GetMeshData(mesh);
	}

	bool AssetMeshProvider::IsMeshLoading(AssetHandle mesh)
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		return manager && mesh.IsValid() && manager->GetAssetState(mesh) == AssetState::Loading;
	}

	uint64_t AssetMeshProvider::GetVersion()
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		SyncWithManager(manager);
		const uint64_t contentVersion = manager ? manager->GetContentVersion() : 0;
		if (contentVersion != m_ContentVersion)
		{
			RemoveUnloadedMeshes(manager, m_ContentVersion);
			m_ContentVersion = contentVersion;
		}
		return (m_Epoch << c_ContentVersionBits) | (contentVersion & c_ContentVersionMask);
	}

	bool AssetMeshProvider::GetChangedMeshes(uint64_t version, std::vector<AssetHandle>& outMeshes)
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		SyncWithManager(manager);
		// Versions of another asset manager, or content versions too large to be told apart, cannot be compared.
		if (!manager || (version >> c_ContentVersionBits) != m_Epoch || manager->GetContentVersion() > c_ContentVersionMask)
			return false;
		return manager->GetContentChanges(version & c_ContentVersionMask, outMeshes);
	}

	void AssetMeshProvider::SyncWithManager(const Ref<AssetManagerBase>& manager)
	{
		if (IsSameOwner(m_Manager, manager))
			return;
		m_Manager = manager;
		m_Epoch++;
		m_Entries.clear();
		m_ContentVersion = manager ? manager->GetContentVersion() : 0;
	}

	void AssetMeshProvider::RemoveUnloadedMeshes(const Ref<AssetManagerBase>& manager, uint64_t sinceVersion)
	{
		if (m_Entries.empty())
			return;

		m_ChangedAssets.clear();
		if (manager && manager->GetContentChanges(sinceVersion, m_ChangedAssets))
		{
			for (AssetHandle handle : m_ChangedAssets)
			{
				auto it = m_Entries.find(handle);
				if (it != m_Entries.end() && it->second.Source.expired())
					m_Entries.erase(it);
			}
			return;
		}
		std::erase_if(m_Entries, [](const auto& entry) { return entry.second.Source.expired(); });
	}

}
