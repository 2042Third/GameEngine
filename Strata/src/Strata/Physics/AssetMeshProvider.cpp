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

	}

	Ref<const PhysicsMeshData> AssetMeshProvider::GetMeshData(AssetHandle mesh)
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
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
		RemoveStaleEntries(manager);
		const Ref<const PhysicsMeshData> data = CreateMeshData(static_cast<const Mesh&>(*asset));
		m_Entries[mesh] = Entry { asset, data };
		return data;
	}

	uint64_t AssetMeshProvider::GetVersion()
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		const uint64_t contentVersion = manager ? manager->GetContentVersion() : 0;
		if (m_VersionManager.lock() != manager || contentVersion != m_ManagerContentVersion)
		{
			m_VersionManager = manager;
			m_ManagerContentVersion = contentVersion;
			m_Version++;
			RemoveStaleEntries(manager); // Meshes may have been unloaded
		}
		return m_Version;
	}

	void AssetMeshProvider::RemoveStaleEntries(const Ref<AssetManagerBase>& manager)
	{
		if (m_EntriesManager.lock() != manager)
		{
			m_Entries.clear();
			m_EntriesManager = manager;
			return;
		}
		std::erase_if(m_Entries, [](const auto& entry) { return entry.second.Source.expired(); });
	}

}
