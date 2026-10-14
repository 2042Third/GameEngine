#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Math/AABB.h"
#include "Strata/Renderer/SceneRenderData.h"
#include "Strata/Renderer/SpatialIndex.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <chrono>
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
	class ISkinningPoses;
	class Mesh;
	class Scene;
	struct MeshSkinBinding;
	struct WorldTransformComponent;

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
		// Has a SkinComponent: an animator may pose it (RenderScene::GetSkinAnimator).
		constexpr uint32_t Skin = 1u << 5;
		// Posed: its skinned mesh is drawn from its palette with the skinned pipelines, and its bounds are the pose's
		// (RenderScene::GetPaletteFirst). A Skin proxy without a pose shows the bind pose like any mesh.
		constexpr uint32_t Skinned = 1u << 6;
	}

	struct RenderSceneStats
	{
		uint32_t Proxies = 0;          // Entities with a MeshRendererComponent
		uint32_t IndexedProxies = 0;   // Of those, in the spatial index (see RenderProxyFlags::Indexed)
		uint32_t InstanceSlots = 0;    // Instances (proxy submeshes) with data, of RenderScene::GetInstances
		uint32_t Meshes = 0;           // Distinct mesh handles in use
		uint32_t Materials = 0;        // Distinct material handles in use
		uint32_t LastSyncUpdates = 0;  // Proxies the latest Sync updated
		uint64_t TotalUpdates = 0;     // Proxies every Sync updated, added up (what changed between two of a renderer's frames)
		uint64_t FullRefreshes = 0;    // Syncs that compared every proxy (the scene no longer knew which transforms changed)
		// Meshes the latest Sync compared with the asset manager: those whose objects, sources or recorded bounds changed, new
		// ones, or all of them after the manager changed (or forgot the changes); none in a sync after which nothing loaded,
		// reloaded, was dropped or got other recorded bounds.
		uint32_t LastSyncMeshChecks = 0;
		uint32_t SkinProxies = 0;       // Proxies with a SkinComponent
		uint32_t PosedProxies = 0;      // Of those, posed (RenderProxyFlags::Skinned)
		uint32_t PaletteJoints = 0;     // Joint matrices of the posed proxies' palettes
		uint32_t LastSyncPoses = 0;     // Palettes the latest Sync computed (poses or placements that changed)
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
	// Skinned meshes: a proxy whose entity has a SkinComponent and whose mesh layout has a skin binding is posed when the
	// scene's ISkinningPoses service (Scene/SceneServices.h: the animation system, the editor's pose preview) has a pose for
	// its animator. Sync then computes its palette - per joint of the mesh's skin, the inverse of the proxy's world
	// transform times the animator's, times the joint's model-space transform, times its inverse bind matrix - into a
	// palette range of its own (GetPalette, logged by version like the instances, so renderers upload only what changed),
	// and its posed bounds: the union of each joint's bind-pose extent (MeshSkinBinding::JointBounds) transformed by its
	// palette entry. Palettes are recomputed only when the pose's version or the transform from the animator to the proxy
	// changes, in parallel chunks. Posed proxies carry RenderProxyFlags::Skinned and their instances c_InstanceSkinned with
	// the palette's first joint (InstanceData::SkinPalette); the skinned pipelines blend it in the vertex shader.
	//
	// Readers follow the scene through three bounded change logs, by version: the instances (GetInstanceChanges), the
	// palettes (GetPaletteChanges) and the proxies themselves (GetProxyChanges: placement, flags, mesh slot, instance range,
	// creation and destruction), which the GPU's culling mirrors.
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
		// Sync that reads at most `maxPending` of the queued entities (GetPendingCount): the latest queued; the others stay
		// queued for the next sync. Everything else is synced as by Sync. For building a scene's proxies a share at a time
		// before it is shown (Prepare).
		uint32_t Sync(const Ref<AssetManagerBase>& assets, uint32_t maxPending);
		// Entities whose MeshRendererComponent was created or signaled since the last sync: each at most once, however
		// often it was signaled, so a scene that is changed but not rendered queues no more than its entities.
		uint32_t GetPendingCount() const { return static_cast<uint32_t>(m_Pending.size()); }

		// Builds the proxies of a scene no renderer shows yet (a scene that was loaded and has still to replace the shown
		// one), within `budget` of this call: syncs chunks of its queued entities (Sync with c_PrepareChunk) until none is
		// left or the budget is spent; at least one chunk. Returns true once none is queued. A scene's first sync would build
		// every proxy in one frame (about 36 ms for 100,000 mesh renderers): owners that load scenes call this once per
		// frame until it returns true before they show the scene, so that the frame that shows it builds none. Main thread.
		static bool Prepare(Scene& scene, const Ref<AssetManagerBase>& assets, std::chrono::microseconds budget);
		// The entities Prepare's syncs read at a time: a few hundred microseconds of work.
		static constexpr uint32_t c_PrepareChunk = 2048;
		// What a frame spends preparing a loaded scene (PrepareLoadedScene): 100,000 mesh renderers take about ten frames.
		static constexpr std::chrono::microseconds c_PrepareFrameBudget { 4000 };
		// What a frame uploads of a loaded scene's GPU copy (PrepareLoadedScene): new proxies' records and instances.
		static constexpr uint64_t c_PrepareUploadBytes = 16ull << 20;
		// SceneLoadOptions::Prepare for owners that render the scenes they load (the editor, StrataRuntime with a window):
		// Prepare with the active asset manager and c_PrepareFrameBudget, then, with a renderer, the scene's GPU copy
		// (RenderSceneGPU) within c_PrepareUploadBytes per call; true once both are complete.
		static bool PrepareLoadedScene(Scene& scene);

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

		// Changes with every change of a proxy's placement (transform, bounds, flags, mesh slot) or instance range, creation
		// and destruction. A reader takes it to read the changes after it later: from then on every change is logged again
		// (a proxy changed several times between two reads is logged once).
		uint64_t GetProxyVersion() const
		{
			m_ProxyChangesRead = m_ProxyVersion;
			return m_ProxyVersion;
		}
		// Appends the proxies that changed after proxy version `sinceVersion` (a proxy may appear more than once; a destroyed
		// proxy too, with a null entity). Returns false, appending nothing, when more than c_MaxProxyChanges changes happened
		// since: then any proxy may have changed. The GPU mirror of the scene follows the proxies through it.
		bool GetProxyChanges(uint64_t sinceVersion, std::vector<uint32_t>& outProxies) const;
		static constexpr size_t c_MaxProxyChanges = 65536;

		//////////////////////////////////////////////////////////////////////////
		// Skinned proxies (palettes of joint matrices, see the class comment)
		//////////////////////////////////////////////////////////////////////////

		// The animator entity posing a Skin proxy, as resolved by the last sync (entt::null when there is none).
		entt::entity GetSkinAnimator(uint32_t proxy) const { return m_SkinIndices[proxy] != c_InvalidIndex ? m_Skins[m_SkinIndices[proxy]].Animator : entt::null; }
		// Whether any proxy has a SkinComponent: renderers skip skinning work entirely otherwise.
		bool HasSkinProxies() const { return !m_Skins.empty(); }
		// A posed proxy's palette: GetPaletteCount joint matrices from GetPaletteFirst on in GetPalette.
		uint32_t GetPaletteFirst(uint32_t proxy) const { return m_SkinIndices[proxy] != c_InvalidIndex ? m_Skins[m_SkinIndices[proxy]].PaletteFirst : 0u; }
		uint32_t GetPaletteCount(uint32_t proxy) const { return m_SkinIndices[proxy] != c_InvalidIndex ? m_Skins[m_SkinIndices[proxy]].PaletteCount : 0u; }
		std::span<const RenderData::AffineTransform> GetPalette() const { return m_Palette; }
		// Changes with every change of the palettes.
		uint64_t GetPaletteVersion() const { return m_PaletteVersion; }
		// Appends the joint ranges (first, count) whose matrices changed after palette version `sinceVersion`. Returns false,
		// appending nothing, when more than c_MaxPaletteChanges changes happened since: then any matrix may have changed.
		bool GetPaletteChanges(uint64_t sinceVersion, std::vector<std::pair<uint32_t, uint32_t>>& outRanges) const;
		static constexpr size_t c_MaxPaletteChanges = 65536;

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
			Ref<const MeshSkinBinding> Skin;                   // Skinned meshes: their binding (part of the layout)
			uint32_t SkinJointsNeeded = 0;                     // One past the largest skeleton joint the binding names
		};

		struct PaletteChange
		{
			uint64_t Version = 0;
			uint32_t First = 0;
			uint32_t Count = 0;
		};

		// The skinning state of a Skin proxy (only those have one, so other proxies carry nothing of it).
		struct SkinEntry
		{
			uint32_t Proxy = 0;
			UUID AnimatorID = UUID::Null();        // SkinComponent::Animator
			entt::entity Animator = entt::null;    // Resolved by the last pose update
			uint32_t PaletteFirst = 0;             // Posed proxies: their palette range in m_Palette
			uint32_t PaletteCount = 0;
			uint64_t PoseVersion = 0;              // The pose version the palette was computed from (0: compute again)
			glm::mat4 AnimatorToProxy = glm::mat4(1.0f); // The transform it was computed with
			// The skin binding it was computed with: another mesh of the same skin (or a reimport) has as many joints but other
			// inverse bind matrices and extents. Held, so a new binding never takes the address of the one it replaces.
			Ref<const MeshSkinBinding> Binding;
			AABB PosedBounds;                      // Local bounds of the pose
		};

		// What a Skin proxy's pose comes from right now (EvaluatePose).
		struct PoseInputs
		{
			bool Posed = false;
			entt::entity Animator = entt::null;
			std::span<const glm::mat4> Joints;
			uint64_t Version = 0;
			glm::mat4 AnimatorToProxy = glm::mat4(1.0f); // Inverse of the proxy's world transform times the animator's
			AABB Reach;              // SkeletonPose::Reach
			float ReachScale = 0.0f;
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
		// SkinComponents created, signaled or destroyed: the proxy reads them at the next sync.
		void OnSkinChanged(entt::registry& registry, entt::entity entity);
		// Queues the entity for the next sync, once; a destroyed one leaves the queue.
		void QueuePending(entt::entity entity);
		void DropPending(entt::entity entity);

		// Creates or rebuilds the proxy from the entity's components; returns whether anything changed.
		bool UpdateProxyFromEntity(entt::entity entity);
		uint32_t CreateProxy(entt::entity entity);
		void DestroyProxy(uint32_t proxy);
		// Reads the world transform and activity; returns whether they differed from the proxy's.
		bool UpdateProxyTransform(uint32_t proxy, const glm::mat4& world, bool active, bool force);
		// Where the entity is drawn: its world transform, or the one the poses' provider draws it at instead
		// (ISkinningPoses::GetRenderTransforms, read by UpdateRenderTransforms).
		const glm::mat4& GetDrawnTransform(entt::entity entity, const WorldTransformComponent& world) const;
		// Takes the provider's render transforms when they changed and places the proxies they add, move or give back.
		void UpdateRenderTransforms(const ISkinningPoses* poses);
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
		// Logs a change of the proxy (GetProxyChanges); a proxy logged since the latest read of the log is logged once.
		void RecordProxyChange(uint32_t proxy);

		// Skinned proxies (see the class comment). Adds or removes the proxy's skin entry as its Skin flag says.
		void SetSkinAnimator(uint32_t proxy, UUID animator);
		// Computes the palettes of the Skin proxies whose pose or placement changed (in parallel chunks) and puts those that
		// lost their pose back in the bind pose.
		void UpdateSkinnedPoses();
		// What the proxy's pose comes from now: posed only for an active proxy with a skinned mesh layout, a resolvable
		// animator entity, a pose from the scene's ISkinningPoses with every joint the skin names, and invertible transforms.
		PoseInputs EvaluatePose(uint32_t proxy, const ISkinningPoses* poses) const;
		// The palette (per skin joint: animatorToProxy * joint * inverse bind matrix) and the posed local bounds.
		static void ComputePalette(const MeshSkinBinding& skin, const PoseInputs& pose, RenderData::AffineTransform* outPalette, AABB& outBounds);
		void ReleasePalette(SkinEntry& skin);
		uint32_t AllocatePalette(uint32_t count);
		void RecordPaletteChange(uint32_t first, uint32_t count);
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

		// The proxy change log (GetProxyChanges), kept like the instance changes.
		uint64_t m_ProxyVersion = 0;
		uint64_t m_ProxyChangesFloor = 0;
		std::vector<InstanceChange> m_ProxyChanges; // Instance: the proxy
		size_t m_ProxyChangesStart = 0;
		std::vector<uint64_t> m_ProxyLoggedVersions; // Per proxy: the version of its newest entry in the log
		// The newest version a reader has taken (GetProxyVersion, GetProxyChanges): changes after it are not read yet.
		mutable uint64_t m_ProxyChangesRead = 0;

		// Skinned proxies: per proxy its entry in m_Skins (c_InvalidIndex without a SkinComponent)
		std::vector<uint32_t> m_SkinIndices;
		std::vector<SkinEntry> m_Skins;
		std::vector<RenderData::AffineTransform> m_Palette;
		std::unordered_map<uint32_t, std::vector<uint32_t>> m_FreePaletteRanges; // Free first joints, by range length
		uint64_t m_PaletteVersion = 0;
		uint64_t m_PaletteChangesFloor = 0;
		std::vector<PaletteChange> m_PaletteChanges;
		size_t m_PaletteChangesStart = 0;
		struct PoseWork
		{
			uint32_t Skin = 0; // Into m_Skins
			PoseInputs Inputs;
		};
		std::vector<PoseWork> m_PoseWork;
		// Entities the poses' provider draws elsewhere than the scene has them (the editor's pose preview: attachments at
		// their joints), as of its render transforms' version.
		std::unordered_map<entt::entity, glm::mat4> m_RenderTransforms;
		const ISkinningPoses* m_RenderTransformsProvider = nullptr;
		uint64_t m_RenderTransformsVersion = 0;
		// What the palettes were last computed against (UpdateSkinnedPoses skips the pass while all of it is unchanged).
		bool m_SkinDirty = false;
		const ISkinningPoses* m_PosesProvider = nullptr;
		uint64_t m_PosesVersion = 0;
		uint64_t m_SkinTransformsVersion = 0;
		uint64_t m_SkinHierarchyVersion = 0;

		RenderSceneStats m_Stats;
		uint64_t m_SyncCount = 0;
		uint32_t m_SyncUpdates = 0;
		std::vector<entt::entity> m_ChangeScratch;
		std::vector<entt::entity> m_PendingScratch;

		friend struct RenderSceneContext;
	};

}
