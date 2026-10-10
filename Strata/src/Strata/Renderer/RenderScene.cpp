#include "stpch.h"
#include "Strata/Renderer/RenderScene.h"

#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/Assert.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		std::atomic<uint64_t> s_NextRenderSceneId = 1;

		// Transforms flatter than this (in volume) draw nothing, as in SceneRenderer before render proxies.
		constexpr float c_MinDeterminant = 1.0e-12f;

		bool IsFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; column++)
			{
				for (int row = 0; row < 4; row++)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		bool IsFiniteBox(const AABB& box)
		{
			return std::isfinite(box.Min.x) && std::isfinite(box.Min.y) && std::isfinite(box.Min.z) && std::isfinite(box.Max.x) && std::isfinite(box.Max.y)
				&& std::isfinite(box.Max.z);
		}

		bool SameBytes(const void* a, const void* b, size_t size)
		{
			return std::memcmp(a, b, size) == 0;
		}

		template<typename T>
		bool SameBytes(const T& a, const T& b)
		{
			return SameBytes(&a, &b, sizeof(T));
		}

		AssetHandle GetSubmeshMaterial(const Submesh& submesh)
		{
			return submesh.Material.IsValid() ? submesh.Material : BuiltinAssets::DefaultMaterial;
		}

	}

	// Holds a scene's render scene in its registry's context. The context is destroyed after the registry's storages, with
	// their signals, so the render scene never disconnects its listeners: there is nothing left to disconnect them from.
	struct RenderSceneContext
	{
		Scope<RenderScene> Instance;
	};

	RenderScene& RenderScene::Get(Scene& scene)
	{
		entt::registry& registry = scene.GetRegistry();
		if (RenderSceneContext* context = registry.ctx().find<RenderSceneContext>())
			return *context->Instance;
		RenderSceneContext& context = registry.ctx().emplace<RenderSceneContext>();
		context.Instance = Scope<RenderScene>(new RenderScene(scene));
		return *context.Instance;
	}

	RenderScene* RenderScene::Find(Scene& scene)
	{
		RenderSceneContext* context = scene.GetRegistry().ctx().find<RenderSceneContext>();
		return context ? context->Instance.get() : nullptr;
	}

	RenderScene::RenderScene(Scene& scene)
		: m_Id(s_NextRenderSceneId.fetch_add(1, std::memory_order_relaxed)), m_Scene(&scene), m_TransformsVersion(scene.GetTransformsVersion())
	{
		entt::registry& registry = scene.GetRegistry();
		// The connections are never released (see RenderSceneContext): this object lives exactly as long as the storages.
		registry.on_construct<MeshRendererComponent>().connect<&RenderScene::OnMeshRendererChanged>(*this);
		registry.on_update<MeshRendererComponent>().connect<&RenderScene::OnMeshRendererChanged>(*this);
		registry.on_destroy<MeshRendererComponent>().connect<&RenderScene::OnMeshRendererDestroyed>(*this);
		// Every mesh renderer the scene already has gets its proxy at the first sync.
		for (const entt::entity entity : registry.view<MeshRendererComponent>())
			m_Pending.push_back(entity);
	}

	RenderScene::~RenderScene() = default;

	void RenderScene::OnMeshRendererChanged(entt::registry&, entt::entity entity)
	{
		// Read at the next sync, so values written right after AddComponent (in the same frame) are seen.
		m_Pending.push_back(entity);
	}

	void RenderScene::OnMeshRendererDestroyed(entt::registry&, entt::entity entity)
	{
		const uint32_t proxy = FindProxy(entity);
		if (proxy != c_InvalidIndex)
			DestroyProxy(proxy);
	}

	uint32_t RenderScene::FindProxy(entt::entity entity) const
	{
		const size_t index = static_cast<size_t>(entt::to_entity(entity));
		if (index >= m_ProxyByEntityIndex.size())
			return c_InvalidIndex;
		const uint32_t proxy = m_ProxyByEntityIndex[index];
		return proxy != c_InvalidIndex && m_Entities[proxy] == entity ? proxy : c_InvalidIndex;
	}

	glm::mat4 RenderScene::GetWorldTransform(uint32_t proxy) const
	{
		return m_Transforms[proxy].ToMatrix();
	}

	AABB RenderScene::ResolveUnloadedBounds(AssetHandle)
	{
		// Until the asset database records the bounds of meshes it has not loaded, a mesh that never loaded is taken to fill
		// one unit around its entity's origin; its proxies grow to the mesh's bounds when it first loads (SetMeshLayout).
		return AABB(glm::vec3(-1.0f), glm::vec3(1.0f));
	}

	uint32_t RenderScene::Sync()
	{
		ST_PROFILE_FUNCTION();
		m_Scene->UpdateWorldTransforms();
		m_SyncCount++;
		m_SyncUpdates = 0;
		entt::registry& registry = m_Scene->GetRegistry();

		m_PendingScratch.clear();
		m_PendingScratch.swap(m_Pending);
		for (const entt::entity entity : m_PendingScratch)
			UpdateProxyFromEntity(entity);

		m_ChangeScratch.clear();
		if (m_Scene->GetWorldTransformChanges(m_TransformsVersion, m_ChangeScratch))
		{
			for (const entt::entity entity : m_ChangeScratch)
			{
				const uint32_t proxy = FindProxy(entity);
				if (proxy == c_InvalidIndex)
					continue;
				const WorldTransformComponent& world = registry.get<WorldTransformComponent>(entity);
				UpdateProxyTransform(proxy, world.Matrix, world.ActiveInHierarchy, false);
			}
		}
		else
		{
			FullRefresh();
		}
		m_TransformsVersion = m_Scene->GetTransformsVersion();
		m_Index.Refresh();
		m_Stats.LastSyncUpdates = m_SyncUpdates;

#ifdef ST_DEBUG
		std::string error;
		if (!Validate(&error))
			ST_CORE_ASSERT(false, "The render proxies of scene '{}' differ from the scene (was a MeshRendererComponent written without a signal?): {}",
				m_Scene->GetName(), error);
#endif
		return m_SyncUpdates;
	}

	void RenderScene::FullRefresh()
	{
		ST_PROFILE_FUNCTION();
		entt::registry& registry = m_Scene->GetRegistry();
		for (uint32_t proxy = 0; proxy < m_Entities.size(); proxy++)
		{
			const entt::entity entity = m_Entities[proxy];
			if (entity == entt::null)
				continue;
			const WorldTransformComponent& world = registry.get<WorldTransformComponent>(entity);
			UpdateProxyTransform(proxy, world.Matrix, world.ActiveInHierarchy, false);
		}
		m_Stats.FullRefreshes++;
	}

	bool RenderScene::UpdateProxyFromEntity(entt::entity entity)
	{
		entt::registry& registry = m_Scene->GetRegistry();
		uint32_t proxy = FindProxy(entity);
		const bool exists = registry.valid(entity) && registry.all_of<MeshRendererComponent, WorldTransformComponent>(entity);
		if (!exists)
		{
			// Destroyed since it was signaled (its proxy went with it), or an entity without a world transform (made
			// directly through the registry), which renders nothing.
			if (proxy != c_InvalidIndex)
				DestroyProxy(proxy);
			return proxy != c_InvalidIndex;
		}

		const MeshRendererComponent& renderer = registry.get<MeshRendererComponent>(entity);
		const WorldTransformComponent& world = registry.get<WorldTransformComponent>(entity);
		const bool created = proxy == c_InvalidIndex;
		if (created)
			proxy = CreateProxy(entity);

		bool changed = created;
		if (created || m_MeshHandles[proxy] != renderer.Mesh)
		{
			if (m_MeshSlots[proxy] != c_InvalidIndex)
				ReleaseMesh(m_MeshSlots[proxy], proxy);
			m_MeshHandles[proxy] = renderer.Mesh;
			m_MeshSlots[proxy] = renderer.Mesh.IsValid() ? AcquireMesh(renderer.Mesh, proxy) : c_InvalidIndex;
			changed = true;
		}
		if (created || m_MaterialOverrides[proxy] != renderer.Material)
		{
			// Acquired before the release, so a slot both use survives.
			const uint32_t previous = m_OverrideSlots[proxy];
			m_MaterialOverrides[proxy] = renderer.Material;
			m_OverrideSlots[proxy] = renderer.Material.IsValid() ? AcquireMaterial(renderer.Material) : c_InvalidIndex;
			if (previous != c_InvalidIndex)
				ReleaseMaterial(previous);
			changed = true;
		}
		const uint32_t castShadows = renderer.CastShadows ? RenderProxyFlags::CastShadows : 0u;
		if ((m_Flags[proxy] & RenderProxyFlags::CastShadows) != castShadows)
		{
			m_Flags[proxy] = (m_Flags[proxy] & ~RenderProxyFlags::CastShadows) | castShadows;
			changed = true;
		}

		if (!changed)
			return UpdateProxyTransform(proxy, world.Matrix, world.ActiveInHierarchy, false);

		// A new proxy, mesh or material: placement, instance slots and their data all follow.
		m_Transforms[proxy] = RenderData::AffineTransform::FromMatrix(world.Matrix);
		m_Flags[proxy] = world.ActiveInHierarchy ? (m_Flags[proxy] | RenderProxyFlags::Active) : (m_Flags[proxy] & ~RenderProxyFlags::Active);
		UpdateProxyPlacement(proxy);
		RebuildInstances(proxy);
		MarkUpdated(proxy);
		return true;
	}

	void RenderScene::MarkUpdated(uint32_t proxy)
	{
		if (m_UpdateStamps[proxy] == m_SyncCount)
			return;
		m_UpdateStamps[proxy] = m_SyncCount;
		m_SyncUpdates++;
	}

	uint32_t RenderScene::CreateProxy(entt::entity entity)
	{
		uint32_t proxy = 0;
		if (!m_FreeProxies.empty())
		{
			proxy = m_FreeProxies.back();
			m_FreeProxies.pop_back();
		}
		else
		{
			proxy = static_cast<uint32_t>(m_Entities.size());
			m_Entities.push_back(entt::null);
			m_EntityIDs.push_back(0);
			m_Flags.push_back(0);
			m_Transforms.emplace_back();
			m_UpdateStamps.push_back(0);
			m_LocalBounds.emplace_back();
			m_WorldBounds.emplace_back();
			m_MeshHandles.push_back(UUID::Null());
			m_MeshSlots.push_back(c_InvalidIndex);
			m_MeshUserPositions.push_back(c_InvalidIndex);
			m_MaterialOverrides.push_back(UUID::Null());
			m_OverrideSlots.push_back(c_InvalidIndex);
			m_FirstInstances.push_back(0);
			m_InstanceCounts.push_back(0);
		}
		m_Entities[proxy] = entity;
		m_EntityIDs[proxy] = static_cast<uint32_t>(entity) + 1;
		m_Flags[proxy] = 0;
		m_Transforms[proxy] = RenderData::AffineTransform::FromMatrix(glm::mat4(1.0f));
		m_LocalBounds[proxy] = AABB();
		m_WorldBounds[proxy] = AABB();
		m_MeshHandles[proxy] = UUID::Null();
		m_MeshSlots[proxy] = c_InvalidIndex;
		m_MaterialOverrides[proxy] = UUID::Null();
		m_OverrideSlots[proxy] = c_InvalidIndex;
		m_FirstInstances[proxy] = 0;
		m_InstanceCounts[proxy] = 0;

		const size_t index = static_cast<size_t>(entt::to_entity(entity));
		if (index >= m_ProxyByEntityIndex.size())
			m_ProxyByEntityIndex.resize(index + 1, c_InvalidIndex);
		m_ProxyByEntityIndex[index] = proxy;
		m_Stats.Proxies++;
		return proxy;
	}

	void RenderScene::DestroyProxy(uint32_t proxy)
	{
		if (m_Index.Contains(proxy))
		{
			m_Index.Remove(proxy);
			m_Stats.IndexedProxies--;
		}
		if (m_InstanceCounts[proxy] > 0)
			FreeInstances(m_FirstInstances[proxy], m_InstanceCounts[proxy]);
		m_InstanceCounts[proxy] = 0;
		if (m_MeshSlots[proxy] != c_InvalidIndex)
			ReleaseMesh(m_MeshSlots[proxy], proxy);
		m_MeshSlots[proxy] = c_InvalidIndex;
		if (m_OverrideSlots[proxy] != c_InvalidIndex)
			ReleaseMaterial(m_OverrideSlots[proxy]);
		m_OverrideSlots[proxy] = c_InvalidIndex;

		m_ProxyByEntityIndex[static_cast<size_t>(entt::to_entity(m_Entities[proxy]))] = c_InvalidIndex;
		m_Entities[proxy] = entt::null;
		m_Flags[proxy] = 0;
		m_FreeProxies.push_back(proxy);
		m_Stats.Proxies--;
		MarkUpdated(proxy);
	}

	bool RenderScene::UpdateProxyTransform(uint32_t proxy, const glm::mat4& world, bool active, bool force)
	{
		const RenderData::AffineTransform transform = RenderData::AffineTransform::FromMatrix(world);
		const bool wasActive = (m_Flags[proxy] & RenderProxyFlags::Active) != 0;
		if (!force && wasActive == active && SameBytes(transform, m_Transforms[proxy]))
			return false;

		m_Transforms[proxy] = transform;
		m_Flags[proxy] = active ? (m_Flags[proxy] | RenderProxyFlags::Active) : (m_Flags[proxy] & ~RenderProxyFlags::Active);
		UpdateProxyPlacement(proxy);
		WriteInstances(proxy);
		MarkUpdated(proxy);
		return true;
	}

	void RenderScene::UpdateProxyPlacement(uint32_t proxy)
	{
		const glm::mat4 world = m_Transforms[proxy].ToMatrix();
		const float determinant = glm::determinant(glm::mat3(world));
		const bool active = (m_Flags[proxy] & RenderProxyFlags::Active) != 0;
		const uint32_t meshSlot = m_MeshSlots[proxy];
		const bool layoutKnown = meshSlot != c_InvalidIndex && m_Meshes[meshSlot].LayoutKnown;

		uint32_t flags = m_Flags[proxy] & (RenderProxyFlags::Active | RenderProxyFlags::CastShadows);
		if (determinant < 0.0f)
			flags |= RenderProxyFlags::Mirrored;
		if (layoutKnown)
			flags |= RenderProxyFlags::MeshBounds;
		m_LocalBounds[proxy] = layoutKnown ? m_Meshes[meshSlot].Bounds : ResolveUnloadedBounds(m_MeshHandles[proxy]);

		// Zero scale and non-finite transforms show nothing.
		const bool drawable = active && meshSlot != c_InvalidIndex && std::isfinite(determinant) && std::abs(determinant) >= c_MinDeterminant && IsFiniteMatrix(world);
		m_WorldBounds[proxy] = drawable ? m_LocalBounds[proxy].Transformed(world) : AABB();
		const bool indexed = drawable && m_WorldBounds[proxy].IsValid() && IsFiniteBox(m_WorldBounds[proxy]);
		if (indexed)
		{
			flags |= RenderProxyFlags::Indexed;
			if (m_Index.Contains(proxy))
			{
				m_Index.Update(proxy, m_WorldBounds[proxy]);
			}
			else
			{
				m_Index.Insert(proxy, m_WorldBounds[proxy]);
				m_Stats.IndexedProxies++;
			}
		}
		else if (m_Index.Contains(proxy))
		{
			m_Index.Remove(proxy);
			m_Stats.IndexedProxies--;
		}
		m_Flags[proxy] = flags;
	}

	uint32_t RenderScene::AcquireMesh(AssetHandle handle, uint32_t proxy)
	{
		uint32_t slot = 0;
		if (auto it = m_MeshSlotsByHandle.find(handle); it != m_MeshSlotsByHandle.end())
		{
			slot = it->second;
		}
		else
		{
			if (!m_FreeMeshes.empty())
			{
				slot = m_FreeMeshes.back();
				m_FreeMeshes.pop_back();
			}
			else
			{
				slot = static_cast<uint32_t>(m_Meshes.size());
				m_Meshes.emplace_back();
			}
			MeshEntry& entry = m_Meshes[slot];
			entry = MeshEntry();
			entry.Handle = handle;
			entry.WithoutLayoutPosition = static_cast<uint32_t>(m_MeshesWithoutLayout.size());
			m_MeshesWithoutLayout.push_back(slot);
			m_MeshSlotsByHandle.emplace(handle, slot);
			m_Stats.Meshes++;
		}
		MeshEntry& entry = m_Meshes[slot];
		m_MeshUserPositions[proxy] = static_cast<uint32_t>(entry.Users.size());
		entry.Users.push_back(proxy);
		return slot;
	}

	void RenderScene::ReleaseMesh(uint32_t slot, uint32_t proxy)
	{
		MeshEntry& entry = m_Meshes[slot];
		const uint32_t position = m_MeshUserPositions[proxy];
		const uint32_t moved = entry.Users.back();
		entry.Users[position] = moved;
		m_MeshUserPositions[moved] = position;
		entry.Users.pop_back();
		m_MeshUserPositions[proxy] = c_InvalidIndex;
		if (!entry.Users.empty())
			return;

		if (entry.WithoutLayoutPosition != c_InvalidIndex)
		{
			const uint32_t movedSlot = m_MeshesWithoutLayout.back();
			m_MeshesWithoutLayout[entry.WithoutLayoutPosition] = movedSlot;
			m_Meshes[movedSlot].WithoutLayoutPosition = entry.WithoutLayoutPosition;
			m_MeshesWithoutLayout.pop_back();
		}
		for (const uint32_t materialSlot : entry.SubmeshMaterialSlots)
			ReleaseMaterial(materialSlot);
		m_MeshSlotsByHandle.erase(entry.Handle);
		entry = MeshEntry();
		m_FreeMeshes.push_back(slot);
		m_Stats.Meshes--;
	}

	uint32_t RenderScene::AcquireMaterial(AssetHandle handle)
	{
		if (auto it = m_MaterialSlotsByHandle.find(handle); it != m_MaterialSlotsByHandle.end())
		{
			m_Materials[it->second].References++;
			return it->second;
		}
		uint32_t slot = 0;
		if (!m_FreeMaterials.empty())
		{
			slot = m_FreeMaterials.back();
			m_FreeMaterials.pop_back();
		}
		else
		{
			slot = static_cast<uint32_t>(m_Materials.size());
			m_Materials.emplace_back();
		}
		m_Materials[slot] = MaterialEntry { handle, 1 };
		m_MaterialSlotsByHandle.emplace(handle, slot);
		m_Stats.Materials++;
		return slot;
	}

	void RenderScene::ReleaseMaterial(uint32_t slot)
	{
		MaterialEntry& entry = m_Materials[slot];
		ST_CORE_ASSERT(entry.References > 0, "RenderScene: material slot {} released more often than acquired", slot);
		if (--entry.References > 0)
			return;
		m_MaterialSlotsByHandle.erase(entry.Handle);
		entry = MaterialEntry();
		m_FreeMaterials.push_back(slot);
		m_Stats.Materials--;
	}

	uint32_t RenderScene::SetMeshLayout(uint32_t slot, const Mesh& mesh)
	{
		ST_PROFILE_FUNCTION();
		MeshEntry& entry = m_Meshes[slot];
		std::vector<AssetHandle> materials;
		materials.reserve(mesh.GetSubmeshes().size());
		for (const Submesh& submesh : mesh.GetSubmeshes())
			materials.push_back(GetSubmeshMaterial(submesh));
		if (entry.LayoutKnown && SameBytes(entry.Bounds, mesh.GetBounds()) && entry.SubmeshMaterials == materials)
		{
			entry.LayoutStale = false;
			return 0;
		}

		// New material slots first, so slots both layouts use survive the release of the old ones.
		std::vector<uint32_t> materialSlots;
		materialSlots.reserve(materials.size());
		for (const AssetHandle material : materials)
			materialSlots.push_back(AcquireMaterial(material));
		for (const uint32_t materialSlot : entry.SubmeshMaterialSlots)
			ReleaseMaterial(materialSlot);
		entry.SubmeshMaterials = std::move(materials);
		entry.SubmeshMaterialSlots = std::move(materialSlots);
		entry.Bounds = mesh.GetBounds();
		entry.LayoutStale = false;
		if (!entry.LayoutKnown)
		{
			entry.LayoutKnown = true;
			const uint32_t movedSlot = m_MeshesWithoutLayout.back();
			m_MeshesWithoutLayout[entry.WithoutLayoutPosition] = movedSlot;
			m_Meshes[movedSlot].WithoutLayoutPosition = entry.WithoutLayoutPosition;
			m_MeshesWithoutLayout.pop_back();
			entry.WithoutLayoutPosition = c_InvalidIndex;
		}

		for (const uint32_t proxy : m_Meshes[slot].Users)
		{
			UpdateProxyPlacement(proxy);
			RebuildInstances(proxy);
		}
		m_Index.Refresh();
		return static_cast<uint32_t>(m_Meshes[slot].Users.size());
	}

	void RenderScene::OnAssetsChanged(std::span<const AssetHandle> handles, bool all)
	{
		if (all)
		{
			for (MeshEntry& entry : m_Meshes)
				entry.LayoutStale = entry.LayoutKnown;
			return;
		}
		for (const AssetHandle handle : handles)
		{
			if (auto it = m_MeshSlotsByHandle.find(handle); it != m_MeshSlotsByHandle.end())
				m_Meshes[it->second].LayoutStale = m_Meshes[it->second].LayoutKnown;
		}
	}

	void RenderScene::RebuildInstances(uint32_t proxy)
	{
		const uint32_t meshSlot = m_MeshSlots[proxy];
		const uint32_t count = meshSlot != c_InvalidIndex && m_Meshes[meshSlot].LayoutKnown ? static_cast<uint32_t>(m_Meshes[meshSlot].SubmeshMaterialSlots.size()) : 0;
		if (count != m_InstanceCounts[proxy])
		{
			if (m_InstanceCounts[proxy] > 0)
				FreeInstances(m_FirstInstances[proxy], m_InstanceCounts[proxy]);
			m_FirstInstances[proxy] = count > 0 ? AllocateInstances(count) : 0;
			m_InstanceCounts[proxy] = count;
			if (count > 0)
			{
				// Fresh slots: written in full, whatever they held before.
				for (uint32_t index = 0; index < count; index++)
					m_Instances[m_FirstInstances[proxy] + index] = RenderData::InstanceData {};
				RecordInstanceChanges(m_FirstInstances[proxy], count);
			}
		}
		WriteInstances(proxy);
	}

	void RenderScene::WriteInstances(uint32_t proxy)
	{
		const uint32_t count = m_InstanceCounts[proxy];
		if (count == 0)
			return;
		const uint32_t first = m_FirstInstances[proxy];
		const MeshEntry& mesh = m_Meshes[m_MeshSlots[proxy]];
		const uint32_t overrideSlot = m_OverrideSlots[proxy];
		RenderData::InstanceData data = {};
		data.World = m_Transforms[proxy];
		data.EntityID = m_EntityIDs[proxy];
		data.Flags = (m_Flags[proxy] & RenderProxyFlags::Mirrored) != 0 ? RenderData::c_InstanceMirrored : 0u;
		for (uint32_t index = 0; index < count; index++)
		{
			data.MaterialIndex = overrideSlot != c_InvalidIndex ? overrideSlot : mesh.SubmeshMaterialSlots[index];
			RenderData::InstanceData& instance = m_Instances[first + index];
			if (SameBytes(instance, data))
				continue;
			instance = data;
			RecordInstanceChanges(first + index, 1);
		}
	}

	uint32_t RenderScene::AllocateInstances(uint32_t count)
	{
		if (auto it = m_FreeInstanceRanges.find(count); it != m_FreeInstanceRanges.end() && !it->second.empty())
		{
			const uint32_t first = it->second.back();
			it->second.pop_back();
			m_Stats.InstanceSlots += count;
			return first;
		}
		const uint32_t first = static_cast<uint32_t>(m_Instances.size());
		m_Instances.resize(m_Instances.size() + count);
		m_Stats.InstanceSlots += count;
		return first;
	}

	void RenderScene::FreeInstances(uint32_t first, uint32_t count)
	{
		m_FreeInstanceRanges[count].push_back(first);
		m_Stats.InstanceSlots -= count;
	}

	void RenderScene::RecordInstanceChanges(uint32_t first, uint32_t count)
	{
		m_InstanceVersion++;
		if (count > c_MaxInstanceChanges)
		{
			m_InstanceChanges.clear();
			m_InstanceChangesStart = 0;
			m_InstanceChangesFloor = m_InstanceVersion;
			return;
		}
		const size_t live = m_InstanceChanges.size() - m_InstanceChangesStart;
		if (live + count > c_MaxInstanceChanges)
		{
			// Renderers further behind than the oldest change kept upload everything.
			m_InstanceChangesStart += live + count - c_MaxInstanceChanges;
			m_InstanceChangesFloor = m_InstanceChanges[m_InstanceChangesStart - 1].Version;
		}
		for (uint32_t index = 0; index < count; index++)
			m_InstanceChanges.push_back(InstanceChange { m_InstanceVersion, first + index });
		if (m_InstanceChangesStart >= c_MaxInstanceChanges)
		{
			m_InstanceChanges.erase(m_InstanceChanges.begin(), m_InstanceChanges.begin() + static_cast<ptrdiff_t>(m_InstanceChangesStart));
			m_InstanceChangesStart = 0;
		}
	}

	bool RenderScene::GetInstanceChanges(uint64_t sinceVersion, std::vector<uint32_t>& outInstances) const
	{
		if (sinceVersion >= m_InstanceVersion)
			return true;
		if (sinceVersion < m_InstanceChangesFloor)
			return false;
		for (size_t index = m_InstanceChanges.size(); index > m_InstanceChangesStart && m_InstanceChanges[index - 1].Version > sinceVersion; index--)
			outInstances.push_back(m_InstanceChanges[index - 1].Instance);
		return true;
	}

	bool RenderScene::Validate(std::string* outError) const
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		const entt::registry& registry = m_Scene->GetRegistry();
		uint32_t expected = 0;
		uint32_t indexed = 0;
		for (const auto [entity, renderer, world] : registry.view<MeshRendererComponent, WorldTransformComponent>().each())
		{
			expected++;
			const uint32_t proxy = FindProxy(entity);
			if (proxy == c_InvalidIndex)
				return fail(fmt::format("entity {} has no proxy", static_cast<uint32_t>(entity)));
			if (m_MeshHandles[proxy] != renderer.Mesh || m_MaterialOverrides[proxy] != renderer.Material
				|| ((m_Flags[proxy] & RenderProxyFlags::CastShadows) != 0) != renderer.CastShadows)
				return fail(fmt::format("the proxy of entity {} has another mesh, material or shadow setting than its MeshRendererComponent", static_cast<uint32_t>(entity)));
			if (!SameBytes(m_Transforms[proxy], RenderData::AffineTransform::FromMatrix(world.Matrix))
				|| ((m_Flags[proxy] & RenderProxyFlags::Active) != 0) != world.ActiveInHierarchy)
				return fail(fmt::format("the proxy of entity {} has another world transform or activity than the entity", static_cast<uint32_t>(entity)));

			const uint32_t meshSlot = m_MeshSlots[proxy];
			if ((meshSlot == c_InvalidIndex) != !renderer.Mesh.IsValid() || (meshSlot != c_InvalidIndex && m_Meshes[meshSlot].Handle != renderer.Mesh))
				return fail(fmt::format("the proxy of entity {} has a wrong mesh slot", static_cast<uint32_t>(entity)));
			const uint32_t overrideSlot = m_OverrideSlots[proxy];
			if ((overrideSlot == c_InvalidIndex) != !renderer.Material.IsValid() || (overrideSlot != c_InvalidIndex && m_Materials[overrideSlot].Handle != renderer.Material))
				return fail(fmt::format("the proxy of entity {} has a wrong material slot", static_cast<uint32_t>(entity)));

			const glm::mat4 matrix = world.Matrix;
			const float determinant = glm::determinant(glm::mat3(matrix));
			const bool layoutKnown = meshSlot != c_InvalidIndex && m_Meshes[meshSlot].LayoutKnown;
			const AABB local = layoutKnown ? m_Meshes[meshSlot].Bounds : ResolveUnloadedBounds(renderer.Mesh);
			const bool drawable = world.ActiveInHierarchy && meshSlot != c_InvalidIndex && std::isfinite(determinant) && std::abs(determinant) >= c_MinDeterminant
				&& IsFiniteMatrix(matrix);
			const AABB bounds = drawable ? local.Transformed(matrix) : AABB();
			const bool shouldBeIndexed = drawable && bounds.IsValid() && IsFiniteBox(bounds);
			if (shouldBeIndexed != m_Index.Contains(proxy) || shouldBeIndexed != ((m_Flags[proxy] & RenderProxyFlags::Indexed) != 0))
				return fail(fmt::format("entity {} is {} the spatial index", static_cast<uint32_t>(entity), shouldBeIndexed ? "missing from" : "wrongly in"));
			if (shouldBeIndexed && (!SameBytes(m_Index.GetBounds(proxy), bounds) || !SameBytes(m_WorldBounds[proxy], bounds)))
				return fail(fmt::format("the bounds of entity {} are stale", static_cast<uint32_t>(entity)));
			if (((m_Flags[proxy] & RenderProxyFlags::Mirrored) != 0) != (determinant < 0.0f))
				return fail(fmt::format("the mirroring of entity {} is stale", static_cast<uint32_t>(entity)));
			indexed += shouldBeIndexed ? 1u : 0u;

			const uint32_t instanceCount = layoutKnown ? static_cast<uint32_t>(m_Meshes[meshSlot].SubmeshMaterialSlots.size()) : 0u;
			if (m_InstanceCounts[proxy] != instanceCount)
				return fail(fmt::format("entity {} has {} instances instead of {}", static_cast<uint32_t>(entity), m_InstanceCounts[proxy], instanceCount));
			for (uint32_t index = 0; index < instanceCount; index++)
			{
				const RenderData::InstanceData& instance = m_Instances[m_FirstInstances[proxy] + index];
				const uint32_t material = overrideSlot != c_InvalidIndex ? overrideSlot : m_Meshes[meshSlot].SubmeshMaterialSlots[index];
				if (!SameBytes(instance.World, RenderData::AffineTransform::FromMatrix(matrix)) || instance.MaterialIndex != material
					|| instance.EntityID != static_cast<uint32_t>(entity) + 1 || instance.Flags != (determinant < 0.0f ? RenderData::c_InstanceMirrored : 0u))
					return fail(fmt::format("instance {} of entity {} is stale", index, static_cast<uint32_t>(entity)));
			}
		}
		if (expected != m_Stats.Proxies)
			return fail(fmt::format("{} proxies for {} mesh renderers", m_Stats.Proxies, expected));
		if (indexed != m_Index.GetCount() || indexed != m_Stats.IndexedProxies)
			return fail(fmt::format("the spatial index holds {} proxies instead of {}", m_Index.GetCount(), indexed));
		return true;
	}

}
