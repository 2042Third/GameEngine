#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Math/AABB.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/SpatialIndex.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class Mesh;
	class Scene;

	// Flags of a render proxy (RenderScene::GetProxyFlags).
	namespace RenderProxyFlags
	{
		constexpr uint32_t Active = 1u << 0;      // Active in the hierarchy
		constexpr uint32_t CastShadows = 1u << 1;
		constexpr uint32_t Mirrored = 1u << 2;    // The world transform mirrors (negative determinant)
		constexpr uint32_t Indexed = 1u << 3;     // In the spatial index: active, with a mesh handle and a finite, invertible transform
		// Its bounds are its mesh's (learned from the loaded mesh, or recorded by the asset database). Without it they are a
		// placeholder around the entity (RenderScene::GetPlaceholderBounds), and renderers request the mesh as if in view.
		constexpr uint32_t MeshBounds = 1u << 4;
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
		// Meshes the latest Sync compared with the asset manager: those whose objects, sources or recorded bounds changed, new
		// ones, or all of them after the manager changed (or forgot the changes); none in a sync after which nothing loaded,
		// reloaded, was dropped or got other recorded bounds.
		uint32_t LastSyncMeshChecks = 0;
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
	// a mesh is evicted, so proxies keep their bounds and instance slots. Sync keeps them current with the asset manager
	// whatever the renderers see: the meshes whose objects or sources changed since the previous sync (the manager's
	// content changes, read against the render scene's own content version, so a scene that was not rendered for a while
	// catches up) and the meshes new to the scene are compared with their loaded objects. A mesh that is not loaded keeps
	// its layout while its source is the one the layout was learned from (AssetSourceRevisions); one reimported while it
	// was not loaded loses it, so its new bounds are not culled with the old ones. Until a mesh's layout is known, the
	// bounds the asset database recorded for it stand in (ResolveUnloadedBounds: imported meshes and the meshes of packs
	// have them). A mesh with neither (a mesh the manager does not know, a record without bounds) has unknown bounds
	// (GetMeshesWithoutBounds): its proxies sit in the spatial index with placeholder bounds around their entities, which
	// say nothing about where the mesh is, so renderers request such a mesh as if it were in view while any of its proxies
	// is active (GetIndexedUserCount) - a terrain whose origin is out of view may well fill it. Its layout follows once it
	// loads. Materials and meshes get dense slots shared by all proxies that use them; instance data refers to material
	// slots, whose GPU data each renderer resolves per frame.
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

		// Updates the scene's world transforms, then the proxies of what changed since the last sync, then the mesh layouts
		// from `assets` (the active asset manager; null: layouts change only through SetMeshLayout). Returns the number of
		// proxies updated.
		uint32_t Sync(const Ref<AssetManagerBase>& assets);
		// Entities whose MeshRendererComponent was created or signaled since the last sync: each at most once, however
		// often it was signaled, so a scene that is changed but not rendered queues no more than its entities.
		uint32_t GetPendingCount() const { return static_cast<uint32_t>(m_Pending.size()); }

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
		// Material slot of each submesh (the default material where the submesh has none).
		std::span<const uint32_t> GetSubmeshMaterialSlots(uint32_t slot) const { return m_Meshes[slot].SubmeshMaterialSlots; }
		// Mesh slots in use whose bounds are unknown: no layout learned and no bounds recorded (see the class comment).
		std::span<const uint32_t> GetMeshesWithoutBounds() const { return m_MeshesWithoutBounds; }
		// The mesh's proxies in the spatial index (active, drawable): while there are none, nothing needs the mesh.
		uint32_t GetIndexedUserCount(uint32_t slot) const { return m_Meshes[slot].IndexedUsers; }
		// Learns (or updates) the layout of a mesh from a loaded object: the bounds and instance slots of every proxy that
		// uses it follow. Returns the number of proxies updated. `sourceRevision` is the asset manager's revision of the
		// source the object was read from (AssetSourceRevisions::Loaded); a sync that finds the mesh not loaded keeps the
		// layout only while the source still has that revision (0, unknown: only while the manager does not know the mesh).
		uint32_t SetMeshLayout(uint32_t slot, const Mesh& mesh, uint64_t sourceRevision = 0);

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

		// The local bounds the asset database recorded for a mesh that `assets` has not loaded (for the layout that is not
		// known yet), or nullopt when it recorded none: AssetManagerBase::GetBounds, from the editor's import records and from
		// a game's pack tables. Meshes known that way are culled and requested where they are instead of as if in view; a
		// change of the recorded bounds is a content change of the mesh, which the next Sync reads.
		static std::optional<AABB> ResolveUnloadedBounds(const AssetManagerBase& assets, AssetHandle mesh);
		// Local bounds of a proxy whose mesh bounds are unknown: a box of one unit around the entity's origin, scaled with
		// it. It keeps the proxy in the spatial index (queries near the entity find it) but says nothing about the mesh.
		static AABB GetPlaceholderBounds();
	private:
		struct MeshEntry
		{
			AssetHandle Handle = UUID::Null();
			std::vector<uint32_t> Users;       // Proxies using the mesh
			uint32_t IndexedUsers = 0;         // Users in the spatial index (RenderProxyFlags::Indexed)
			bool LayoutKnown = false;
			uint64_t LayoutRevision = 0;       // The source revision the layout was learned from (see SetMeshLayout)
			AABB Bounds;
			std::vector<AssetHandle> SubmeshMaterials;
			std::vector<uint32_t> SubmeshMaterialSlots;
			std::optional<AABB> RecordedBounds;                // While the layout is unknown: ResolveUnloadedBounds
			uint32_t WithoutBoundsPosition = c_InvalidIndex;   // In m_MeshesWithoutBounds
			uint64_t CheckedSync = 0;                          // The sync that last compared the layout with the loaded mesh
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
		// Queues the entity for the next sync, once; a destroyed one leaves the queue.
		void QueuePending(entt::entity entity);
		void DropPending(entt::entity entity);

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

		// Mesh layouts from the asset manager (see Sync): the meshes whose objects or sources changed and new mesh slots.
		void UpdateMeshLayouts(const Ref<AssetManagerBase>& assets);
		// Learns the layout from the loaded mesh; for a mesh that is not loaded, forgets a layout learned from another source
		// revision and reads the recorded bounds of a mesh whose layout is not known.
		void CheckMeshLayout(uint32_t slot, AssetManagerBase& assets);
		// Drops a known layout (its submesh material slots too); the caller places the users again.
		void ForgetMeshLayout(uint32_t slot);
		// Adds the slot to, or removes it from, m_MeshesWithoutBounds as its layout and recorded bounds say.
		void UpdateBoundsMembership(uint32_t slot);

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
		std::vector<uint32_t> m_PendingPositions; // By entt::to_entity: the entity's place in m_Pending

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
		std::vector<uint32_t> m_MeshesWithoutBounds;
		std::vector<uint32_t> m_NewMeshSlots; // Mesh slots acquired since the last sync: their meshes may be loaded already
		// The asset manager the layouts follow, and its content version they are current with.
		std::weak_ptr<AssetManagerBase> m_Assets;
		uint64_t m_AssetContentVersion = 0;
		std::vector<AssetHandle> m_AssetChangeScratch;
		std::vector<uint32_t> m_MeshCheckScratch;
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
