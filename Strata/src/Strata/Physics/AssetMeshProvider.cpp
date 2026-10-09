#include "stpch.h"
#include "Strata/Physics/AssetMeshProvider.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Renderer/Mesh.h"

namespace Strata
{

	namespace
	{

		// The full-detail triangles of every submesh, with indices into the whole vertex list.
		Ref<const PhysicsMeshData> CreateMeshData(const Mesh& mesh)
		{
			const std::vector<uint32_t>& indices = mesh.GetIndices();
			const auto isValidRange = [&indices](const MeshLOD& lod)
			{
				return static_cast<uint64_t>(lod.IndexOffset) + lod.IndexCount <= indices.size();
			};

			Ref<PhysicsMeshData> data = CreateRef<PhysicsMeshData>();
			data->Positions = mesh.GetPositions();
			size_t indexCount = 0;
			for (const Submesh& submesh : mesh.GetSubmeshes())
			{
				if (!submesh.LODs.empty() && isValidRange(submesh.LODs.front()))
					indexCount += submesh.LODs.front().IndexCount;
			}
			data->Indices.reserve(indexCount);

			// Mesh validates its ranges when it is created; the check only keeps a broken object from being read out of bounds
			// (vertex indices are validated when the shape is cooked).
			for (const Submesh& submesh : mesh.GetSubmeshes())
			{
				if (submesh.LODs.empty() || !isValidRange(submesh.LODs.front()))
					continue;
				const MeshLOD& lod = submesh.LODs.front();
				for (uint32_t index = lod.IndexOffset; index < lod.IndexOffset + lod.IndexCount; index++)
					data->Indices.push_back(submesh.BaseVertex + indices[index]);
			}
			return data;
		}

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
		if (!asset || asset->GetType() != AssetType::Mesh)
			return nullptr;

		auto it = m_Entries.find(mesh);
		if (it != m_Entries.end() && it->second.Source.lock() == asset)
			return it->second.Data;

		// First use, or a new object (reloaded): new data, so that physics rebuilds the colliders using it.
		const Ref<const PhysicsMeshData> data = CreateMeshData(static_cast<const Mesh&>(*asset));
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
