#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Math/AABB.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/SpatialIndex.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Mesh;
	class Scene;

	// Flags of a render proxy (RenderScene::GetProxyFlags).
	namespace RenderProxyFlags
	{
		constexpr uint32_t Active = 1u << 0;      // Active in the hierarchy
		constexpr uint32_t CastShadows = 1u << 1;
		constexpr uint32_t Mirrored = 1u << 2;    // The world transform mirrors (negative determinant)
		constexpr uint32_t Indexed = 1u << 3;     // In the spatial index: active, with a mesh handle and a finite, invertible transform
		constexpr uint32_t MeshBounds = 1u << 4;  // Its bounds come from its mesh (otherwise from ResolveUnloadedBounds)
	}

	struct RenderSceneStats
	{
		uint32_t Proxies = 0;          // Entities with a MeshRendererComponent
		uint32_t IndexedProxies = 0;   // Of those, in the spatial index (see RenderProxyFlags::Indexed)
		uint32_t InstanceSlots = 0;    // Instances (proxy submeshes) with data, of RenderScene::GetInstances
		uint32_t Meshes = 0;           // Distinct mesh handles in use
		uint32_t Materials = 0;        // Distinct material handles in use
		uint32_t LastSyncUpdates = 0;  // Proxies the latest Sync updated
		uint64_t FullRefreshes = 0;    // Syncs that compared every proxy (the scene no longer knew which transforms changed)
	};

	// The renderer's persistent view of a scene's mesh renderers: one render proxy per entity with a MeshRendererComponent,
	// kept in structure-of-arrays form (world transform, world bounds, mesh, material override, flags, entity ID) and in a
	// spatial index, plus the GPU instance data of every proxy submesh whose mesh layout is known. Sync updates only what
	// changed since the previous sync: entities whose MeshRendererComponent was created, signaled (on_update) or destroyed,
	// and entities whose world transform or activity changed (Scene::GetWorldTransformChanges; a full comparison when the
	// scene no longer knows). The contract is the transform contract's: code that writes MeshRendererComponent fields of an
	// entity created before the last sync must signal it (Entity::MarkModified, registry.patch; ComponentAccess does);
	// Debug builds validate the proxies after every Sync (Validate).
	//
	// Mesh layouts (bounds, submeshes and their materials) are learned from loaded meshes (SetMeshLayout) and kept when
	// a mesh is evicted, so proxies keep their bounds and instance slots; a mesh whose layout was never learned bounds its
	// proxies with ResolveUnloadedBounds. Materials and meshes get dense slots shared by all proxies that use them;
	// instance data refers to material slots, whose GPU data each renderer resolves per frame.
	//
	// One per scene, created on first use (Get) in the scene's registry context and shared by every renderer of the scene
	// (the viewport and capture renderers). Main thread only.
	class RenderScene
	{
	public:
		static constexpr uint32_t c_InvalidIndex = ~0u;
		// Instance changes remembered for renderers that are behind (GetInstanceChanges).
		static constexpr size_t c_MaxInstanceChanges = 65536;

		// The scene's render scene, created on first use. It lives as long as the scene's registry.
		static RenderScene& Get(Scene& scene);
		// The scene's render scene if one was created.
		static RenderScene* Find(Scene& scene);

		~RenderScene();

		RenderScene(const RenderScene&) = delete;
		RenderScene& operator=(const RenderScene&) = delete;

		// Unique in the process, so renderers can tell render scenes apart (a new scene may reuse an address).
		uint64_t GetId() const { return m_Id; }

		// Updates the scene's world transforms, then the proxies of what changed since the last sync. Returns the number of
		// proxies updated.
		uint32_t Sync();

		// Compares every proxy, the spatial index and the instance data with what a rebuild from the scene would produce.
		// Returns false with the first difference.
		bool Validate(std::string* outError = nullptr) const;

		//////////////////////////////////////////////////////////////////////////
		// Proxies (indices below GetProxyCapacity; free ones have a null entity)
		//////////////////////////////////////////////////////////////////////////

		uint32_t GetProxyCapacity() const { return static_cast<uint32_t>(m_Entities.size()); }
		uint32_t FindProxy(entt::entity entity) const;
		entt::entity GetEntity(uint32_t proxy) const { return m_Entities[proxy]; }
		uint32_t GetEntityID(uint32_t proxy) const { return m_EntityIDs[proxy]; } // Entity handle + 1, as the ID buffer stores it
		uint32_t GetProxyFlags(uint32_t proxy) const { return m_Flags[proxy]; }
		glm::mat4 GetWorldTransform(uint32_t proxy) const;
		const AABB& GetWorldBounds(uint32_t proxy) const { return m_WorldBounds[proxy]; }
		const AABB& GetLocalBounds(uint32_t proxy) const { return m_LocalBounds[proxy]; }
		uint32_t GetMeshSlot(uint32_t proxy) const { return m_MeshSlots[proxy]; }
		AssetHandle GetMaterialOverride(uint32_t proxy) const { return m_MaterialOverrides[proxy]; }
		// The proxy's instances: one per submesh of its mesh, consecutive; none until the mesh layout is known.
		uint32_t GetFirstInstance(uint32_t proxy) const { return m_FirstInstances[proxy]; }
		uint32_t GetInstanceCount(uint32_t proxy) const { return m_InstanceCounts[proxy]; }

		const SpatialIndex& GetSpatialIndex() const { return m_Index; }

		//////////////////////////////////////////////////////////////////////////
		// Meshes and materials
		//////////////////////////////////////////////////////////////////////////

		uint32_t GetMeshSlotCapacity() const { return static_cast<uint32_t>(m_Meshes.size()); }
		AssetHandle GetMeshHandle(uint32_t slot) const { return m_Meshes[slot].Handle; }
		bool IsMeshLayoutKnown(uint32_t slot) const { return m_Meshes[slot].LayoutKnown; }
		// The mesh's object changed since its layout was learned (a reload or an eviction): the next loaded object is
		// compared with it.
		bool IsMeshLayoutStale(uint32_t slot) const { return m_Meshes[slot].LayoutStale; }
		// Material slot of each submesh (the default material where the submesh has none).
		std::span<const uint32_t> GetSubmeshMaterialSlots(uint32_t slot) const { return m_Meshes[slot].SubmeshMaterialSlots; }
		// Mesh slots in use whose layout is not known yet.
		std::span<const uint32_t> GetMeshesWithoutLayout() const { return m_MeshesWithoutLayout; }
		// Learns (or updates) the layout of a mesh from a loaded object: the bounds and instance slots of every proxy that
		// uses it follow. Returns the number of proxies updated.
		uint32_t SetMeshLayout(uint32_t slot, const Mesh& mesh);
		// Assets whose loaded objects changed (AssetManagerBase::GetContentChanges): the layouts of those meshes become
		// stale. `all`: any asset may have changed.
		void OnAssetsChanged(std::span<const AssetHandle> handles, bool all);

		uint32_t GetMaterialSlotCapacity() const { return static_cast<uint32_t>(m_Materials.size()); }
		AssetHandle GetMaterialHandle(uint32_t slot) const { return m_Materials[slot].Handle; }
		// Material slot of a proxy's override (c_InvalidIndex without one).
		uint32_t GetOverrideMaterialSlot(uint32_t proxy) const { return m_OverrideSlots[proxy]; }

		//////////////////////////////////////////////////////////////////////////
		// Instance data (RenderData::InstanceData per proxy submesh)
		//////////////////////////////////////////////////////////////////////////

		std::span<const RenderData::InstanceData> GetInstances() const { return m_Instances; }
		// Changes with every change of the instance data.
		uint64_t GetInstanceVersion() const { return m_InstanceVersion; }
		// Appends the instances whose data changed after instance version `sinceVersion` (an instance may appear more than
		// once). Returns false, appending nothing, when more than c_MaxInstanceChanges changes happened since: then any
		// instance may have changed.
		bool GetInstanceChanges(uint64_t sinceVersion, std::vector<uint32_t>& outInstances) const;

		const RenderSceneStats& GetStats() const { return m_Stats; }

		// Local bounds of a proxy whose mesh layout is not known: a box of one unit around the entity's origin, scaled with
		// it, which grows to the mesh's bounds when the mesh first loads.
		static AABB ResolveUnloadedBounds(AssetHandle mesh);
	private:
		struct MeshEntry
		{
			AssetHandle Handle = UUID::Null();
			std::vector<uint32_t> Users;       // Proxies using the mesh
			bool LayoutKnown = false;
			bool LayoutStale = false;
			uint32_t WithoutLayoutPosition = c_InvalidIndex; // In m_MeshesWithoutLayout
			AABB Bounds;
			std::vector<AssetHandle> SubmeshMaterials;
			std::vector<uint32_t> SubmeshMaterialSlots;
		};

		struct MaterialEntry
		{
			AssetHandle Handle = UUID::Null();
			uint32_t References = 0; // Proxy overrides and mesh layouts naming it
		};

		struct InstanceChange
		{
			uint64_t Version = 0;
			uint32_t Instance = 0;
		};

		explicit RenderScene(Scene& scene);

		void OnMeshRendererChanged(entt::registry& registry, entt::entity entity);
		void OnMeshRendererDestroyed(entt::registry& registry, entt::entity entity);

		// Creates or rebuilds the proxy from the entity's components; returns whether anything changed.
		bool UpdateProxyFromEntity(entt::entity entity);
		uint32_t CreateProxy(entt::entity entity);
		void DestroyProxy(uint32_t proxy);
		// Reads the world transform and activity; returns whether they differed from the proxy's.
		bool UpdateProxyTransform(uint32_t proxy, const glm::mat4& world, bool active, bool force);
		// Bounds, index membership and flags from the proxy's transform, mesh layout and activity.
		void UpdateProxyPlacement(uint32_t proxy);
		// Counts the proxy as updated by the current sync (once).
		void MarkUpdated(uint32_t proxy);
		void FullRefresh();

		uint32_t AcquireMesh(AssetHandle handle, uint32_t proxy);
		void ReleaseMesh(uint32_t slot, uint32_t proxy);
		uint32_t AcquireMaterial(AssetHandle handle);
		void ReleaseMaterial(uint32_t slot);

		// Instance slots of the proxy for its mesh layout (released when the layout is unknown), and their data.
		void RebuildInstances(uint32_t proxy);
		void WriteInstances(uint32_t proxy);
		uint32_t AllocateInstances(uint32_t count);
		void FreeInstances(uint32_t first, uint32_t count);
		void RecordInstanceChanges(uint32_t first, uint32_t count);
	private:
		uint64_t m_Id = 0;
		Scene* m_Scene = nullptr;
		uint64_t m_TransformsVersion = 0;
		std::vector<entt::entity> m_Pending; // Entities whose MeshRendererComponent was created or signaled since the last sync

		// Proxies (structure of arrays)
		std::vector<entt::entity> m_Entities;
		std::vector<uint32_t> m_EntityIDs;
		std::vector<uint32_t> m_Flags;
		std::vector<RenderData::AffineTransform> m_Transforms;
		std::vector<uint64_t> m_UpdateStamps; // The sync that last updated the proxy (each proxy counts once per sync)
		std::vector<AABB> m_LocalBounds;
		std::vector<AABB> m_WorldBounds;
		std::vector<AssetHandle> m_MeshHandles;
		std::vector<uint32_t> m_MeshSlots;
		std::vector<uint32_t> m_MeshUserPositions;
		std::vector<AssetHandle> m_MaterialOverrides;
		std::vector<uint32_t> m_OverrideSlots;
		std::vector<uint32_t> m_FirstInstances;
		std::vector<uint32_t> m_InstanceCounts;
		std::vector<uint32_t> m_FreeProxies;
		std::vector<uint32_t> m_ProxyByEntityIndex; // By entt::to_entity of the entity
		SpatialIndex m_Index;

		std::vector<MeshEntry> m_Meshes;
		std::vector<uint32_t> m_FreeMeshes;
		std::unordered_map<AssetHandle, uint32_t> m_MeshSlotsByHandle;
		std::vector<uint32_t> m_MeshesWithoutLayout;
		std::vector<MaterialEntry> m_Materials;
		std::vector<uint32_t> m_FreeMaterials;
		std::unordered_map<AssetHandle, uint32_t> m_MaterialSlotsByHandle;

		std::vector<RenderData::InstanceData> m_Instances;
		std::unordered_map<uint32_t, std::vector<uint32_t>> m_FreeInstanceRanges; // Free first instances, by range length
		uint64_t m_InstanceVersion = 0;
		uint64_t m_InstanceChangesFloor = 0; // Changes up to this version may be missing from m_InstanceChanges
		std::vector<InstanceChange> m_InstanceChanges;
		size_t m_InstanceChangesStart = 0;   // Entries before this are dropped (compacted when they make up half the list)

		RenderSceneStats m_Stats;
		uint64_t m_SyncCount = 0;
		uint32_t m_SyncUpdates = 0;
		std::vector<entt::entity> m_ChangeScratch;
		std::vector<entt::entity> m_PendingScratch;

		friend struct RenderSceneContext;
	};

}
