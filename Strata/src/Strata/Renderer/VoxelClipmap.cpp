#include "stpch.h"
#include "Strata/Renderer/VoxelClipmap.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace Strata
{

	namespace
	{

		uint64_t HashBytes(const void* data, size_t size, uint64_t hash = 1469598103934665603ull)
		{
			const uint8_t* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; index++)
				hash = (hash ^ bytes[index]) * 1099511628211ull;
			return hash;
		}

		// The proxy flags that change what the voxels take of a proxy.
		constexpr uint32_t c_VoxelizedFlags = RenderProxyFlags::Active | RenderProxyFlags::Indexed | RenderProxyFlags::Skinned | RenderProxyFlags::Mirrored;

		// A world box as the spatial index's planes (inward normals): x, y and z between their minimum and maximum.
		std::array<glm::vec4, 6> GetBoxPlanes(const glm::dvec3& minimum, const glm::dvec3& maximum)
		{
			const glm::vec3 low(minimum);
			const glm::vec3 high(maximum);
			return { {
				{ 1.0f, 0.0f, 0.0f, -low.x },
				{ -1.0f, 0.0f, 0.0f, high.x },
				{ 0.0f, 0.0f, 1.0f, -low.z },
				{ 0.0f, 0.0f, -1.0f, high.z },
				{ 0.0f, -1.0f, 0.0f, high.y },
				{ 0.0f, 1.0f, 0.0f, -low.y },
			} };
		}

		bool IntersectsBox(const AABB& bounds, const glm::dvec3& minimum, const glm::dvec3& maximum)
		{
			return bounds.IsValid() && bounds.Max.x >= minimum.x && bounds.Max.y >= minimum.y && bounds.Max.z >= minimum.z && bounds.Min.x <= maximum.x
				&& bounds.Min.y <= maximum.y && bounds.Min.z <= maximum.z;
		}

		VoxelBox Shrink(const VoxelBox& box, int64_t voxels)
		{
			return VoxelBox { box.Minimum + voxels, box.Maximum - voxels };
		}

	}

	void VoxelFrame::Clear()
	{
		Regions.clear();
		Items.clear();
		Groups.clear();
		Batches.clear();
		Triangles = 0;
		QueuedRegions = 0;
		WaitingProxies = 0;
		MovingProxies = 0;
		UnvoxelizedSubmeshes = 0;
		Pending = false;
		Rebuilt = false;
		MaterialsChanged = false;
	}

	VoxelClipmap::VoxelClipmap(GlobalIlluminationLayout layout)
		: m_Layout(std::move(layout))
	{
		m_Levels.resize(m_Layout.Levels.size());
	}

	VoxelBox VoxelClipmap::GetTouchedVoxels(const AABB& bounds, float voxelSize)
	{
		if (!bounds.IsValid())
			return VoxelBox {};
		const double size = static_cast<double>(voxelSize);
		const glm::dvec3 low = glm::ceil(glm::dvec3(bounds.Min) / size);
		const glm::dvec3 high = glm::floor(glm::dvec3(bounds.Max) / size);
		// Voxel i's closed box [i, i + 1] meets [min, max] for ceil(min) - 1 <= i <= floor(max); one more each way for the GPU's
		// rounding of the same positions in floats.
		VoxelBox box;
		box.Minimum = glm::i64vec3(static_cast<int64_t>(low.x), static_cast<int64_t>(low.y), static_cast<int64_t>(low.z)) - int64_t(2);
		box.Maximum = glm::i64vec3(static_cast<int64_t>(high.x), static_cast<int64_t>(high.y), static_cast<int64_t>(high.z)) + int64_t(2);
		return box;
	}

	void VoxelClipmap::BuildFrame(const RenderScene& renderScene, const IVoxelSceneSource& source, const glm::dvec3& cameraPosition,
		const VoxelClipmapSettings& settings, VoxelFrame& outFrame)
	{
		ST_PROFILE_FUNCTION();
		outFrame.Clear();
		m_Frame++;
		const bool sameScene = m_Started && (renderScene.GetId() == m_SceneId || renderScene.GetCloneSource() == m_SceneId);
		bool cut = false;
		if (!sameScene)
		{
			StartOver(renderScene, source, cameraPosition);
			outFrame.Rebuilt = true;
			outFrame.MaterialsChanged = true;
			cut = true;
		}
		else
		{
			m_SceneId = renderScene.GetId();
			FollowCamera(cameraPosition, cut);
			FollowTables(renderScene, source, outFrame);
			FollowProxies(renderScene, source);
		}

		// The frame's regions: changes first (a moved object never waits behind slabs), then slabs, then whole windows, each
		// level 0 first, while their triangles fit the budget; at least one region.
		const uint32_t budget = cut ? settings.TrianglesOnCut : settings.TrianglesPerFrame;
		std::vector<TakenRegion> taken;
		uint64_t triangles = 0;
		uint64_t items = 0;
		uint64_t groups = 0;
		bool full = false;
		for (size_t kind = 0; kind < c_RegionKinds && !full; kind++)
		{
			for (uint32_t level = 0; level < m_Levels.size() && !full; level++)
			{
				std::deque<VoxelBox>& queue = m_Levels[level].Queues[kind];
				while (!queue.empty())
				{
					if (taken.size() >= RenderData::c_GIMaxRegions)
					{
						full = true;
						break;
					}
					TakenRegion region;
					uint32_t unvoxelized = 0;
					const VoxelBox box = queue.front();
					if (!TakeRegion(renderScene, level, box, region, unvoxelized))
					{
						// More work than a frame holds in one region: halve it along its longest axis.
						queue.pop_front();
						const glm::i64vec3 size = box.Maximum - box.Minimum;
						const int axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
						VoxelBox lower = box;
						VoxelBox upper = box;
						lower.Maximum[axis] = box.Minimum[axis] + size[axis] / 2;
						upper.Minimum[axis] = lower.Maximum[axis];
						queue.push_front(upper);
						queue.push_front(lower);
						continue;
					}
					uint64_t regionGroups = 0;
					for (uint32_t itemTriangles : region.ItemTriangles)
						regionGroups += (itemTriangles + RenderData::c_GIGroupTriangles - 1) / RenderData::c_GIGroupTriangles;
					const bool overBudget = budget != 0 && triangles + region.Triangles > budget;
					const bool overLimits = items + region.Items.size() > RenderData::c_GIMaxWorkItems || groups + regionGroups > RenderData::c_GIMaxTriangleGroups;
					if (!taken.empty() && (overBudget || overLimits))
					{
						full = true;
						break;
					}
					queue.pop_front();
					triangles += region.Triangles;
					items += region.Items.size();
					groups += regionGroups;
					outFrame.UnvoxelizedSubmeshes += unvoxelized;
					taken.push_back(std::move(region));
				}
			}
		}
		outFrame.Triangles = static_cast<uint32_t>(std::min<uint64_t>(triangles, UINT32_MAX));
		EmitFrame(taken, outFrame);

		// A level is valid in its whole window once no slab or fill of it waits; until then it keeps what it held.
		for (uint32_t level = 0; level < m_Levels.size(); level++)
		{
			Level& state = m_Levels[level];
			const VoxelBox window = GlobalIllumination::GetWindow(m_Layout.Levels[level], state.State.Window);
			if (state.Queues[static_cast<size_t>(RegionKind::Slab)].empty() && state.Queues[static_cast<size_t>(RegionKind::Fill)].empty())
				state.State.Valid = window;
			for (const std::deque<VoxelBox>& queue : state.Queues)
				outFrame.QueuedRegions += static_cast<uint32_t>(queue.size());
			// Pending while levels 0-2 miss their windows beyond two scroll steps (a rebuild or a jump under way), never for a
			// slab or a change left for later: samples read the next level there.
			if (level < 3 && !state.State.Valid.Contains(Shrink(window, 2 * static_cast<int64_t>(RenderData::c_GIScrollVoxels))))
				outFrame.Pending = true;
		}
		// The lists hold each proxy once from here on (Admit may have listed one twice).
		for (std::vector<uint32_t>* list : { &m_Waiting, &m_Moving })
		{
			std::sort(list->begin(), list->end());
			list->erase(std::unique(list->begin(), list->end()), list->end());
		}
		for (uint32_t proxy : m_Waiting)
			outFrame.WaitingProxies += m_Proxies[proxy].Status == ProxyStatus::Waiting ? 1u : 0u;
		for (uint32_t proxy : m_Moving)
			outFrame.MovingProxies += m_Proxies[proxy].Status == ProxyStatus::Moving ? 1u : 0u;
	}

	void VoxelClipmap::StartOver(const RenderScene& renderScene, const IVoxelSceneSource& source, const glm::dvec3& cameraPosition)
	{
		m_Started = true;
		m_SceneId = renderScene.GetId();
		for (uint32_t level = 0; level < m_Levels.size(); level++)
		{
			Level& state = m_Levels[level];
			for (std::deque<VoxelBox>& queue : state.Queues)
				queue.clear();
			state.State.Window = GlobalIllumination::GetWindowMinimum(m_Layout.Levels[level], cameraPosition);
			state.State.Anchor = GlobalIllumination::GetAnchorVoxel(state.State.Window);
			state.State.Valid = VoxelBox {};
			QueueWindow(level, RegionKind::Fill);
		}
		m_Meshes.clear();
		m_Materials.clear();
		m_Proxies.clear();
		m_Waiting.clear();
		m_Moving.clear();
		// The tables first (proxies are ready by their meshes), then every proxy as it is: the windows are queued whole.
		VoxelFrame unused;
		m_TablesVersion = source.GetTablesVersion() + 1;
		FollowTables(renderScene, source, unused);
		m_ProxyVersion = renderScene.GetProxyVersion();
		Rescan(renderScene, source, false);
	}

	void VoxelClipmap::FollowCamera(const glm::dvec3& cameraPosition, bool& outCut)
	{
		std::vector<VoxelBox> slabs;
		for (uint32_t level = 0; level < m_Levels.size(); level++)
		{
			Level& state = m_Levels[level];
			const VoxelLevelLayout& layout = m_Layout.Levels[level];
			const glm::i64vec3 window = GlobalIllumination::GetWindowMinimum(layout, cameraPosition);
			if (window != state.State.Window)
			{
				slabs.clear();
				const VoxelBox box = GlobalIllumination::GetWindow(layout, window);
				if (GlobalIllumination::GetScrollSlabs(state.State.Window, window, layout.Dimensions, slabs))
				{
					// What the window keeps stays valid; queued boxes keep their part within it.
					state.State.Window = window;
					state.State.Valid = state.State.Valid.Intersection(box);
					if (state.State.Valid.IsEmpty())
						state.State.Valid = VoxelBox {};
					for (std::deque<VoxelBox>& queue : state.Queues)
					{
						std::deque<VoxelBox> kept;
						for (const VoxelBox& queued : queue)
						{
							const VoxelBox inside = queued.Intersection(box);
							if (!inside.IsEmpty())
								kept.push_back(inside);
						}
						queue = std::move(kept);
					}
					for (const VoxelBox& slab : slabs)
						state.Queues[static_cast<size_t>(RegionKind::Slab)].push_back(slab);
				}
				else
				{
					// A move of a window or more: the window starts over (its anchor kept while near).
					state.State.Window = window;
					state.State.Valid = VoxelBox {};
					for (std::deque<VoxelBox>& queue : state.Queues)
						queue.clear();
					QueueWindow(level, RegionKind::Fill);
					outCut = true;
				}
			}
			// The anchor follows a camera that went c_GIAnchorMeters beyond its anchor's cell on an axis (a cell of
			// c_GIAnchorVoxels voxels: 102 m at 10 cm, 819 m at 80 cm); the window is voxelized again relative to the new one, its
			// voxels valid meanwhile. Coordinates relative to it stay within a few ten thousand voxels: floats keep a thousandth.
			const double voxelSize = static_cast<double>(layout.VoxelSize);
			const glm::dvec3 anchor = glm::dvec3(state.State.Anchor) * voxelSize;
			const double reach = RenderData::c_GIAnchorMeters + static_cast<double>(RenderData::c_GIAnchorVoxels) * voxelSize;
			if (glm::any(glm::greaterThan(glm::abs(cameraPosition - anchor), glm::dvec3(reach))))
			{
				state.State.Anchor = GlobalIllumination::GetAnchorVoxel(state.State.Window);
				if (state.Queues[static_cast<size_t>(RegionKind::Fill)].empty())
				{
					state.Queues[static_cast<size_t>(RegionKind::Refresh)].clear();
					QueueWindow(level, RegionKind::Refresh);
				}
			}
		}
	}

	void VoxelClipmap::FollowTables(const RenderScene& renderScene, const IVoxelSceneSource& source, VoxelFrame& outFrame)
	{
		const uint32_t meshSlots = renderScene.GetMeshSlotCapacity();
		const uint32_t materialSlots = renderScene.GetMaterialSlotCapacity();
		const uint64_t version = source.GetTablesVersion();
		if (version == m_TablesVersion && meshSlots == m_Meshes.size() && materialSlots == m_Materials.size())
			return;
		m_TablesVersion = version;

		// Materials whose voxel form changed: their users' voxels are made again.
		std::vector<uint32_t> changedMaterials;
		if (m_Materials.size() != materialSlots)
			outFrame.MaterialsChanged = true;
		m_Materials.resize(materialSlots, RenderData::GIMaterial {});
		for (uint32_t slot = 0; slot < materialSlots; slot++)
		{
			const RenderData::GIMaterial material = source.GetMaterial(slot);
			if (std::memcmp(&material, &m_Materials[slot], sizeof(material)) != 0)
			{
				m_Materials[slot] = material;
				changedMaterials.push_back(slot);
				outFrame.MaterialsChanged = true;
			}
		}

		// Meshes whose GPU entry changed (loaded, reloaded, dropped): their layouts again, and their users' voxels.
		std::vector<uint8_t> changedMeshes(meshSlots, 0);
		bool anyMesh = false;
		m_Meshes.resize(meshSlots);
		for (uint32_t slot = 0; slot < meshSlots; slot++)
		{
			const uint32_t entry = source.GetMeshTableEntry(slot);
			MeshInfo& info = m_Meshes[slot];
			if (entry == info.Entry)
				continue;
			info = MeshInfo {};
			info.Entry = entry;
			changedMeshes[slot] = 1;
			anyMesh = true;
			const Ref<const Mesh> mesh = entry != 0 ? source.GetMesh(slot) : nullptr;
			if (!mesh)
				continue;
			info.Closed = mesh->IsClosed();
			for (const Submesh& submesh : mesh->GetSubmeshes())
			{
				MeshInfo::SubmeshInfo& part = info.Submeshes.emplace_back();
				part.Bounds = submesh.Bounds;
				part.HasLODErrors = MeshUtils::HasLODErrors(submesh);
				for (const MeshLOD& lod : submesh.LODs)
				{
					part.LODTriangles.push_back(lod.IndexCount / 3);
					part.LODErrors.push_back(lod.Error);
				}
			}
		}

		if (!m_Started || m_Proxies.empty())
			return;
		std::vector<uint32_t> users;
		if (!changedMaterials.empty())
			renderScene.CollectMaterialUsers(changedMaterials, users);
		if (anyMesh)
		{
			// Rare (a mesh loaded or dropped): every proxy's mesh slot is read.
			for (uint32_t proxy = 0; proxy < renderScene.GetProxyCapacity(); proxy++)
			{
				const uint32_t slot = renderScene.GetMeshSlot(proxy);
				if (slot != RenderScene::c_InvalidIndex && slot < meshSlots && changedMeshes[slot])
					users.push_back(proxy);
			}
		}
		std::sort(users.begin(), users.end());
		users.erase(std::unique(users.begin(), users.end()), users.end());
		for (uint32_t proxy : users)
			Refresh(renderScene, source, proxy);
	}

	void VoxelClipmap::FollowProxies(const RenderScene& renderScene, const IVoxelSceneSource& source)
	{
		m_Scratch.clear();
		if (!renderScene.GetProxyChanges(m_ProxyVersion, m_Scratch))
		{
			// More changes than the log keeps: every proxy as it is now, every window again (still valid meanwhile).
			m_ProxyVersion = renderScene.GetProxyVersion();
			Rescan(renderScene, source, true);
		}
		else
		{
			m_ProxyVersion = renderScene.GetProxyVersion();
			std::sort(m_Scratch.begin(), m_Scratch.end());
			m_Scratch.erase(std::unique(m_Scratch.begin(), m_Scratch.end()), m_Scratch.end());
			if (m_Proxies.size() < renderScene.GetProxyCapacity())
				m_Proxies.resize(renderScene.GetProxyCapacity());
			for (uint32_t proxy : m_Scratch)
			{
				if (proxy >= m_Proxies.size())
					continue;
				ProxyState& state = m_Proxies[proxy];
				const bool drawable = proxy < renderScene.GetProxyCapacity() && IsDrawable(renderScene, proxy);
				const uint64_t placement = drawable ? GetPlacementHash(renderScene, proxy) : 0;
				const uint64_t content = drawable ? GetContentHash(renderScene, proxy) : 0;
				switch (state.Status)
				{
				case ProxyStatus::Voxelized:
					if (drawable && content == state.Content)
					{
						if (placement != state.Placement)
						{
							// Moved: out of the voxels until it rests (a move a frame would re-voxelize every frame).
							QueueBounds(state.Bounds);
							state.Status = ProxyStatus::Moving;
							state.MovedFrame = m_Frame;
							m_Moving.push_back(proxy);
						}
					}
					else
					{
						QueueBounds(state.Bounds);
						state.Status = ProxyStatus::None;
						Admit(renderScene, source, proxy, true);
					}
					break;
				case ProxyStatus::Moving:
					if (drawable && content == state.Content)
					{
						if (placement != state.Placement)
							state.MovedFrame = m_Frame;
					}
					else
					{
						state.Status = ProxyStatus::None;
						Admit(renderScene, source, proxy, true);
					}
					break;
				default:
					state.Status = ProxyStatus::None;
					Admit(renderScene, source, proxy, true);
					break;
				}
				state.Placement = placement;
				state.Content = content;
			}
		}

		// Proxies that came to rest, and those that became resident with their mesh.
		std::vector<uint32_t> list;
		std::swap(list, m_Moving);
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end()), list.end());
		for (uint32_t proxy : list)
		{
			if (proxy >= m_Proxies.size() || m_Proxies[proxy].Status != ProxyStatus::Moving)
				continue;
			if (m_Frame - m_Proxies[proxy].MovedFrame >= RenderData::c_GIMovingFrames)
			{
				m_Proxies[proxy].Status = ProxyStatus::None;
				Admit(renderScene, source, proxy, true);
			}
			else
			{
				m_Moving.push_back(proxy);
			}
		}
		list.clear();
		std::swap(list, m_Waiting);
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end()), list.end());
		for (uint32_t proxy : list)
		{
			if (proxy < m_Proxies.size() && m_Proxies[proxy].Status == ProxyStatus::Waiting)
				Admit(renderScene, source, proxy, true);
		}
	}

	void VoxelClipmap::Rescan(const RenderScene& renderScene, const IVoxelSceneSource& source, bool queueWindows)
	{
		if (queueWindows)
		{
			for (uint32_t level = 0; level < m_Levels.size(); level++)
			{
				m_Levels[level].Queues[static_cast<size_t>(RegionKind::Refresh)].clear();
				if (m_Levels[level].Queues[static_cast<size_t>(RegionKind::Fill)].empty())
					QueueWindow(level, RegionKind::Refresh);
			}
		}
		m_Proxies.assign(renderScene.GetProxyCapacity(), ProxyState {});
		m_Waiting.clear();
		m_Moving.clear();
		for (uint32_t proxy = 0; proxy < renderScene.GetProxyCapacity(); proxy++)
		{
			ProxyState& state = m_Proxies[proxy];
			if (!IsDrawable(renderScene, proxy))
				continue;
			state.Placement = GetPlacementHash(renderScene, proxy);
			state.Content = GetContentHash(renderScene, proxy);
			Admit(renderScene, source, proxy, false);
		}
	}

	bool VoxelClipmap::IsDrawable(const RenderScene& renderScene, uint32_t proxy) const
	{
		if (renderScene.GetEntity(proxy) == entt::null)
			return false;
		const uint32_t flags = renderScene.GetProxyFlags(proxy);
		return (flags & RenderProxyFlags::Active) != 0 && (flags & RenderProxyFlags::Indexed) != 0 && (flags & RenderProxyFlags::Skinned) == 0
			&& renderScene.GetMeshSlot(proxy) != RenderScene::c_InvalidIndex;
	}

	bool VoxelClipmap::IsReady(const RenderScene& renderScene, const IVoxelSceneSource& source, uint32_t proxy) const
	{
		if (!IsDrawable(renderScene, proxy) || !source.IsResident(proxy) || renderScene.GetInstanceCount(proxy) == 0)
			return false;
		const uint32_t slot = renderScene.GetMeshSlot(proxy);
		if (slot >= m_Meshes.size())
			return false;
		// Meshes of the Skinned arena (a SkinComponent's mesh, posed or not) are not in the voxels: the voxelizer reads the Static
		// arena's positions.
		const MeshInfo& info = m_Meshes[slot];
		return info.Entry != 0 && (info.Entry & RenderData::c_CullMeshSkinnedArena) == 0 && !info.Submeshes.empty();
	}

	uint64_t VoxelClipmap::GetPlacementHash(const RenderScene& renderScene, uint32_t proxy) const
	{
		const glm::mat4 world = renderScene.GetWorldTransform(proxy);
		return HashBytes(&world, sizeof(world));
	}

	uint64_t VoxelClipmap::GetContentHash(const RenderScene& renderScene, uint32_t proxy) const
	{
		const uint32_t values[] = { renderScene.GetMeshSlot(proxy), renderScene.GetOverrideMaterialSlot(proxy), renderScene.GetProxyFlags(proxy) & c_VoxelizedFlags,
			renderScene.GetFirstInstance(proxy), renderScene.GetInstanceCount(proxy) };
		return HashBytes(values, sizeof(values));
	}

	void VoxelClipmap::Admit(const RenderScene& renderScene, const IVoxelSceneSource& source, uint32_t proxy, bool queue)
	{
		ProxyState& state = m_Proxies[proxy];
		// Meshes of the Skinned arena (a SkinComponent's mesh, posed or not) are left out, not waited for: the voxelizer reads
		// the Static arena's positions.
		const uint32_t meshSlot = renderScene.GetMeshSlot(proxy);
		const bool skinnedArena = meshSlot < m_Meshes.size() && (m_Meshes[meshSlot].Entry & RenderData::c_CullMeshSkinnedArena) != 0;
		if (!IsDrawable(renderScene, proxy) || skinnedArena)
		{
			state.Status = ProxyStatus::None;
			return;
		}
		if (!IsReady(renderScene, source, proxy))
		{
			state.Status = ProxyStatus::Waiting;
			m_Waiting.push_back(proxy);
			return;
		}
		state.Status = ProxyStatus::Voxelized;
		state.Bounds = renderScene.GetWorldBounds(proxy);
		if (queue)
			QueueBounds(state.Bounds);
	}

	void VoxelClipmap::Refresh(const RenderScene& renderScene, const IVoxelSceneSource& source, uint32_t proxy)
	{
		if (proxy >= m_Proxies.size())
			return;
		ProxyState& state = m_Proxies[proxy];
		if (state.Status == ProxyStatus::Moving)
			return; // Out of the voxels until it rests, and then put in as it is
		if (state.Status == ProxyStatus::Voxelized)
			QueueBounds(state.Bounds);
		state.Status = ProxyStatus::None;
		Admit(renderScene, source, proxy, true);
	}

	void VoxelClipmap::QueueBounds(const AABB& bounds)
	{
		if (!bounds.IsValid())
			return;
		std::vector<VoxelBox> pieces;
		for (uint32_t level = 0; level < m_Levels.size(); level++)
		{
			const VoxelLevelLayout& layout = m_Layout.Levels[level];
			Level& state = m_Levels[level];
			const VoxelBox window = GlobalIllumination::GetWindow(layout, state.State.Window);
			const VoxelBox box = GlobalIllumination::SnapToBricks(GetTouchedVoxels(bounds, layout.VoxelSize)).Intersection(window);
			if (box.IsEmpty())
				continue;
			pieces.clear();
			GlobalIllumination::SplitRegion(box, layout.Dimensions, pieces);
			std::deque<VoxelBox>& queue = state.Queues[static_cast<size_t>(RegionKind::Change)];
			for (const VoxelBox& piece : pieces)
			{
				// A box a queued change already holds adds nothing.
				if (std::none_of(queue.begin(), queue.end(), [&piece](const VoxelBox& queued) { return queued.Contains(piece); }))
					queue.push_back(piece);
			}
		}
	}

	void VoxelClipmap::QueueWindow(uint32_t level, RegionKind kind)
	{
		std::vector<VoxelBox> pieces;
		const VoxelLevelLayout& layout = m_Layout.Levels[level];
		GlobalIllumination::SplitRegion(GlobalIllumination::GetWindow(layout, m_Levels[level].State.Window), layout.Dimensions, pieces);
		std::deque<VoxelBox>& queue = m_Levels[level].Queues[static_cast<size_t>(kind)];
		queue.insert(queue.end(), pieces.begin(), pieces.end());
	}

	bool VoxelClipmap::TakeRegion(const RenderScene& renderScene, uint32_t level, const VoxelBox& box, TakenRegion& outRegion, uint32_t& outUnvoxelized)
	{
		const VoxelLevelLayout& layout = m_Layout.Levels[level];
		const double voxel = static_cast<double>(layout.VoxelSize);
		// The region's world box, a voxel larger each way: what touches its faces marks voxels inside.
		const glm::dvec3 low = glm::dvec3(box.Minimum) * voxel - voxel;
		const glm::dvec3 high = glm::dvec3(box.Maximum) * voxel + voxel;
		outRegion = TakenRegion {};
		outRegion.Level = level;
		outRegion.Box = box;

		m_Candidates.clear();
		const std::array<glm::vec4, 6> planes = GetBoxPlanes(low, high);
		renderScene.GetSpatialIndex().QueryPlanes(planes, m_Candidates);
		std::sort(m_Candidates.begin(), m_Candidates.end());
		// Closed meshes below the region in its columns: their winding steps decide what inside the region is solid.
		m_Below.clear();
		renderScene.GetSpatialIndex().QueryPlanes(std::span<const glm::vec4>(planes.data(), 5), m_Below);
		std::sort(m_Below.begin(), m_Below.end());

		const float halfVoxel = 0.5f * layout.VoxelSize;
		const auto addProxy = [&](uint32_t proxy, bool inRegion)
		{
			if (proxy >= m_Proxies.size() || m_Proxies[proxy].Status != ProxyStatus::Voxelized)
				return;
			const uint32_t meshSlot = renderScene.GetMeshSlot(proxy);
			if (meshSlot >= m_Meshes.size() || meshSlot > RenderData::c_VoxelItemMaxMeshSlot)
				return;
			const MeshInfo& mesh = m_Meshes[meshSlot];
			const std::span<const uint32_t> submeshMaterials = renderScene.GetSubmeshMaterialSlots(meshSlot);
			const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
			const auto materialOf = [&](size_t submesh) -> uint32_t
			{
				const uint32_t slot = overrideSlot != RenderScene::c_InvalidIndex ? overrideSlot : (submesh < submeshMaterials.size() ? submeshMaterials[submesh] : RenderScene::c_InvalidIndex);
				return slot < m_Materials.size() ? m_Materials[slot].Flags : 0u;
			};
			// Submeshes past c_VoxelItemMaxSubmesh (a work item holds 8 bits of submesh) or past the proxy's instances are left
			// out (counted where the proxy is in the region).
			const uint32_t instances = renderScene.GetInstanceCount(proxy);
			const size_t drawn = std::min<size_t>(mesh.Submeshes.size(), instances);
			const size_t voxelized = std::min<size_t>(drawn, static_cast<size_t>(RenderData::c_VoxelItemMaxSubmesh) + 1);
			// Closed as drawn: a closed mesh whose every submesh is voxelized and opaque (a part left out, glass or cut out would
			// leave holes in its winding, and columns through them would fill solid).
			bool closed = mesh.Closed && voxelized == mesh.Submeshes.size();
			for (size_t submesh = 0; submesh < mesh.Submeshes.size() && closed; submesh++)
			{
				const uint32_t flags = materialOf(submesh);
				closed = (flags & RenderData::c_CullMaterialAvailable) != 0
					&& (flags & (RenderData::c_CullMaterialAlphaMask | RenderData::c_CullMaterialTransparent | RenderData::c_CullMaterialTransmissive)) == 0;
			}
			if (!inRegion && !closed)
				return;
			const glm::mat4 world = renderScene.GetWorldTransform(proxy);
			const float scale = std::max({ glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])) });
			const uint32_t proxyFlags = renderScene.GetProxyFlags(proxy);
			const uint32_t firstInstance = renderScene.GetFirstInstance(proxy);
			if (inRegion)
				outUnvoxelized += static_cast<uint32_t>(drawn - voxelized);
			for (uint32_t submesh = 0; submesh < voxelized; submesh++)
			{
				const uint32_t materialFlags = materialOf(submesh);
				if ((materialFlags & RenderData::c_CullMaterialAvailable) == 0 || (materialFlags & RenderData::c_CullMaterialTransparent) != 0)
					continue;
				const MeshInfo::SubmeshInfo& part = mesh.Submeshes[submesh];
				const AABB bounds = part.Bounds.Transformed(world);
				uint32_t itemFlags = 0;
				if (closed)
				{
					// Steps reach the region from below or within it, in its columns.
					if (!bounds.IsValid() || bounds.Min.y > high.y || bounds.Max.x < low.x || bounds.Min.x > high.x || bounds.Max.z < low.z || bounds.Min.z > high.z)
						continue;
					itemFlags |= RenderData::c_VoxelItemClosed;
					if (!inRegion || !IntersectsBox(bounds, low, high))
						itemFlags |= RenderData::c_VoxelItemFillOnly;
				}
				else if (!IntersectsBox(bounds, low, high))
				{
					continue;
				}
				if ((materialFlags & RenderData::c_CullMaterialAlphaMask) != 0)
					itemFlags |= RenderData::c_VoxelItemAlphaMasked;
				if ((materialFlags & RenderData::c_CullMaterialDoubleSided) != 0)
					itemFlags |= RenderData::c_VoxelItemDoubleSided;
				if ((proxyFlags & RenderProxyFlags::Mirrored) != 0)
					itemFlags |= RenderData::c_VoxelItemMirrored;
				// Open meshes: the coarsest level whose error stays within half a voxel; closed ones keep full detail (a simplified
				// level may open).
				uint32_t lod = 0;
				if (!closed && part.HasLODErrors)
				{
					for (uint32_t candidate = 1; candidate < part.LODErrors.size() && candidate < 16; candidate++)
					{
						if (part.LODErrors[candidate] * scale <= halfVoxel)
							lod = candidate;
					}
				}
				if (lod >= part.LODTriangles.size())
					continue;
				const uint32_t itemTriangles = part.LODTriangles[lod];
				if (itemTriangles == 0)
					continue;
				RenderData::VoxelWorkItem item;
				item.Instance = firstInstance + submesh;
				item.MeshSubmeshLOD = meshSlot | (submesh << 20) | (lod << 28);
				item.Region = 0;
				item.Flags = itemFlags;
				outRegion.Items.push_back(item);
				outRegion.ItemTriangles.push_back(itemTriangles);
				outRegion.ItemProxies.push_back(proxy);
				outRegion.Triangles += itemTriangles;
			}
		};
		for (uint32_t proxy : m_Candidates)
			addProxy(proxy, true);
		for (uint32_t proxy : m_Below)
		{
			if (!std::binary_search(m_Candidates.begin(), m_Candidates.end(), proxy))
				addProxy(proxy, false);
		}

		// A region whose work alone passes a frame's limits is halved, while it can be.
		uint64_t regionGroups = 0;
		for (uint32_t itemTriangles : outRegion.ItemTriangles)
			regionGroups += (itemTriangles + RenderData::c_GIGroupTriangles - 1) / RenderData::c_GIGroupTriangles;
		if (outRegion.Items.size() > RenderData::c_GIMaxWorkItems || regionGroups > RenderData::c_GIMaxTriangleGroups)
		{
			const glm::i64vec3 size = box.Maximum - box.Minimum;
			if (std::max({ size.x, size.y, size.z }) > static_cast<int64_t>(RenderData::c_GIRegionBrick))
				return false;
			// A brick with more work than a frame holds (never in practice): its first items only, the rest counted.
			bool cutClosed = false;
			uint32_t cutProxy = 0;
			while (outRegion.Items.size() > RenderData::c_GIMaxWorkItems || regionGroups > RenderData::c_GIMaxTriangleGroups)
			{
				const uint32_t dropped = outRegion.ItemTriangles.back();
				regionGroups -= (dropped + RenderData::c_GIGroupTriangles - 1) / RenderData::c_GIGroupTriangles;
				cutClosed = (outRegion.Items.back().Flags & RenderData::c_VoxelItemClosed) != 0;
				cutProxy = outRegion.ItemProxies.back();
				outRegion.Triangles -= dropped;
				outRegion.Items.pop_back();
				outRegion.ItemTriangles.pop_back();
				outRegion.ItemProxies.pop_back();
				outUnvoxelized++;
			}
			// A closed mesh cut in two would step into its columns unbalanced: what is left of it (the last items, a proxy's
			// items being together) is voxelized open, its steps from below dropped.
			if (cutClosed)
			{
				size_t kept = outRegion.Items.size();
				while (kept > 0 && outRegion.ItemProxies[kept - 1] == cutProxy)
					kept--;
				for (size_t index = kept; index < outRegion.Items.size(); index++)
				{
					RenderData::VoxelWorkItem& item = outRegion.Items[index];
					if ((item.Flags & RenderData::c_VoxelItemFillOnly) != 0)
					{
						outRegion.Triangles -= outRegion.ItemTriangles[index];
						continue;
					}
					item.Flags &= ~RenderData::c_VoxelItemClosed;
					outRegion.Items[kept] = item;
					outRegion.ItemTriangles[kept] = outRegion.ItemTriangles[index];
					outRegion.ItemProxies[kept] = outRegion.ItemProxies[index];
					kept++;
				}
				outRegion.Items.resize(kept);
				outRegion.ItemTriangles.resize(kept);
				outRegion.ItemProxies.resize(kept);
			}
		}
		return true;
	}

	void VoxelClipmap::EmitFrame(std::vector<TakenRegion>& taken, VoxelFrame& outFrame) const
	{
		// Batches hold one level's regions: the GPU passes of a batch write that level's textures.
		std::stable_sort(taken.begin(), taken.end(), [](const TakenRegion& a, const TakenRegion& b) { return a.Level < b.Level; });
		VoxelFrameBatch* batch = nullptr;
		for (TakenRegion& region : taken)
		{
			const VoxelLevelLayout& layout = m_Layout.Levels[region.Level];
			const LevelState& state = m_Levels[region.Level].State;
			const glm::i64vec3 size = region.Box.Maximum - region.Box.Minimum;
			const uint32_t voxels = static_cast<uint32_t>(region.Box.GetVoxelCount());
			if (!batch || batch->Level != region.Level || batch->Voxels + voxels > m_Layout.BatchVoxels)
			{
				VoxelFrameBatch& added = outFrame.Batches.emplace_back();
				added.Level = region.Level;
				added.FirstRegion = static_cast<uint32_t>(outFrame.Regions.size());
				added.FirstGroup = static_cast<uint32_t>(outFrame.Groups.size());
				batch = &added;
			}
			const glm::i64vec3 minimum = region.Box.Minimum - state.Anchor;
			const glm::i64vec3 maximum = region.Box.Maximum - state.Anchor;
			const glm::uvec3 texel = GlobalIllumination::GetTexel(region.Box.Minimum, layout.Dimensions);
			RenderData::VoxelRegion record;
			record.Minimum = glm::ivec4(static_cast<int32_t>(minimum.x), static_cast<int32_t>(minimum.y), static_cast<int32_t>(minimum.z), static_cast<int32_t>(region.Level));
			record.Maximum = glm::ivec4(static_cast<int32_t>(maximum.x), static_cast<int32_t>(maximum.y), static_cast<int32_t>(maximum.z), static_cast<int32_t>(batch->Voxels));
			record.Texel = glm::ivec4(glm::ivec3(texel), static_cast<int32_t>(batch->Columns));
			const uint32_t regionIndex = static_cast<uint32_t>(outFrame.Regions.size());
			outFrame.Regions.push_back(record);
			for (size_t index = 0; index < region.Items.size(); index++)
			{
				RenderData::VoxelWorkItem item = region.Items[index];
				item.Region = regionIndex;
				const uint32_t itemIndex = static_cast<uint32_t>(outFrame.Items.size());
				outFrame.Items.push_back(item);
				for (uint32_t first = 0; first < region.ItemTriangles[index]; first += RenderData::c_GIGroupTriangles)
					outFrame.Groups.push_back(RenderData::VoxelBatch { itemIndex, first });
			}
			batch->RegionCount++;
			batch->Voxels += voxels;
			batch->Columns += static_cast<uint32_t>(size.x * size.z);
			batch->GroupCount = static_cast<uint32_t>(outFrame.Groups.size()) - batch->FirstGroup;
		}
	}

}
