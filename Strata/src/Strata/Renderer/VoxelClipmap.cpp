#include "stpch.h"
#include "Strata/Renderer/VoxelClipmap.h"

#include "Strata/Core/Assert.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
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

		// The brick of c_GIMoverBrickVoxels voxels a world voxel index lies in, per axis (bricks start at multiples of it, so a
		// mover's bricks stay the same while windows scroll).
		glm::i64vec3 GetMoverBrick(const glm::i64vec3& voxel)
		{
			constexpr int64_t c_Brick = RenderData::c_GIMoverBrickVoxels;
			glm::i64vec3 brick;
			for (int axis = 0; axis < 3; axis++)
				brick[axis] = voxel[axis] >= 0 ? voxel[axis] / c_Brick : -((-voxel[axis] + c_Brick - 1) / c_Brick);
			return brick;
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
		MoversLeftOut = 0;
		Movers.clear();
		MoverBricks.clear();
		MoversChanged = false;
		MoverFades = 0;
		MoverSnapshots = 0;
		UnvoxelizedSubmeshes = 0;
		Pending = false;
		Rebuilt = false;
		Cut = false;
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
			FollowProxies(renderScene, source, settings.StepSeconds);
			// A fade starts from what the textures hold: the voxels that were valid before the frame's regions are taken.
			if (!m_FadeBounds.empty())
			{
				m_FadeValid.resize(m_Levels.size());
				for (uint32_t level = 0; level < m_Levels.size(); level++)
					m_FadeValid[level] = m_Levels[level].State.Valid;
			}
		}

		// The frame's regions: changes first (a moved object never waits behind slabs), then slabs, then whole windows, each
		// level 0 first, while their triangles fit the budget; at least one region.
		outFrame.Cut = cut;
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
		for (std::vector<uint32_t>* list : { &m_Waiting, &m_Moving, &m_Resting })
		{
			std::sort(list->begin(), list->end());
			list->erase(std::unique(list->begin(), list->end()), list->end());
		}
		for (uint32_t proxy : m_Waiting)
			outFrame.WaitingProxies += m_Proxies[proxy].Status == ProxyStatus::Waiting ? 1u : 0u;
		// A proxy that came to rest leaves the dynamic layer in the frame every level took the regions that hold its triangles
		// again (the budget may leave them queued): never a frame in neither.
		std::erase_if(m_Resting, [this](uint32_t proxy)
		{
			return proxy >= m_Proxies.size() || m_Proxies[proxy].Status != ProxyStatus::Voxelized || !IsChangeQueued(m_Proxies[proxy].Bounds);
		});
		EmitMovers(renderScene, source, cameraPosition, settings.StepSeconds, outFrame);
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
		m_LargestAlbedo = 0;
		m_Proxies.clear();
		m_Waiting.clear();
		m_Moving.clear();
		m_Resting.clear();
		m_Fades.clear();
		m_FadeBounds.clear();
		m_MoverBrickCount = 0;
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

	void VoxelClipmap::FollowProxies(const RenderScene& renderScene, const IVoxelSceneSource& source, float stepSeconds)
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
							// Moved: its triangles leave the voxels until it rests (voxelizing them again every frame would also
							// re-dispatch every static triangle around it); the dynamic layer holds it meanwhile (EmitMovers), and
							// the voxels it leaves fade into what takes their place (FollowFades).
							m_FadeBounds.push_back(state.Bounds);
							QueueBounds(state.Bounds);
							state.Status = ProxyStatus::Moving;
							state.MovedFrame = m_Frame;
							state.RestSeconds = 0.0f;
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
						{
							state.MovedFrame = m_Frame;
							state.RestSeconds = 0.0f;
						}
					}
					else
					{
						// Another mesh, material or flags: its triangles are voxelized as it is now, and the layer keeps it until they are.
						state.Status = ProxyStatus::None;
						Admit(renderScene, source, proxy, true);
						if (state.Status == ProxyStatus::Voxelized)
						{
							m_Resting.push_back(proxy);
							m_FadeBounds.push_back(state.Bounds);
						}
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
			// Rest counts the renderer's clock, never frames: the same wait at any frame rate.
			ProxyState& state = m_Proxies[proxy];
			if (state.MovedFrame != m_Frame && std::isfinite(stepSeconds))
				state.RestSeconds += std::max(stepSeconds, 0.0f);
			if (state.RestSeconds >= RenderData::c_GIMoverRestSeconds)
			{
				// Its box fades into the voxels its triangles make (FollowFades).
				state.Status = ProxyStatus::None;
				Admit(renderScene, source, proxy, true);
				if (state.Status == ProxyStatus::Voxelized)
				{
					m_Resting.push_back(proxy);
					m_FadeBounds.push_back(state.Bounds);
				}
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
		m_Resting.clear();
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

	bool VoxelClipmap::IsChangeQueued(const AABB& bounds) const
	{
		if (!bounds.IsValid())
			return false;
		for (uint32_t level = 0; level < m_Levels.size(); level++)
		{
			const VoxelLevelLayout& layout = m_Layout.Levels[level];
			const VoxelBox box = GlobalIllumination::SnapToBricks(GetTouchedVoxels(bounds, layout.VoxelSize));
			for (const VoxelBox& queued : m_Levels[level].Queues[static_cast<size_t>(RegionKind::Change)])
			{
				if (queued.Intersects(box))
					return true;
			}
		}
		return false;
	}

	uint64_t VoxelClipmap::GetMoverKey(uint32_t level, const glm::i64vec3& brick) const
	{
		// A brick that holds a voxel of the window lies at or after the window's first brick, fewer than 2^16 bricks on.
		const glm::i64vec3 relative = brick - GetMoverBrick(m_Levels[level].State.Window);
		return (static_cast<uint64_t>(level) << 48) | (static_cast<uint64_t>(relative.z) << 32) | (static_cast<uint64_t>(relative.y) << 16) | static_cast<uint64_t>(relative.x);
	}

	void VoxelClipmap::FollowFades(float stepSeconds, VoxelFrame& outFrame)
	{
		constexpr int64_t c_Brick = RenderData::c_GIMoverBrickVoxels;
		constexpr size_t c_New = std::numeric_limits<size_t>::max();
		if (!m_Fades.empty() || !m_FadeBounds.empty())
		{
			// The fades' clock is the movers'. A fade ends with its time, or when its brick has left its level's window.
			const float step = std::isfinite(stepSeconds) ? std::max(stepSeconds, 0.0f) : 0.0f;
			for (MoverFade& fade : m_Fades)
			{
				fade.Seconds += step;
				fade.Snapshot = false;
			}
			std::erase_if(m_Fades, [this](const MoverFade& fade)
			{
				const VoxelBox window = GlobalIllumination::GetWindow(m_Layout.Levels[fade.Level], m_Levels[fade.Level].State.Window);
				return fade.Seconds >= RenderData::c_GIMoverFadeSeconds || !window.Intersects(VoxelBox { fade.Brick * c_Brick, fade.Brick * c_Brick + c_Brick });
			});

			// The backup's slots: the frame's bricks keep what they held in the first ones, by their order, and a fade keeps what
			// its brick showed in one counted from the end, for as long as it lasts. `reserved`: the slots from the end up to the
			// last a fade holds; `bricks`: the frame's, the movers' (m_MoverKeys so far) and those that only fade.
			const uint32_t slots = m_Layout.MoverBricks;
			ST_CORE_ASSERT(slots <= RenderData::c_VoxelMoverFadeSlotMask, "VoxelClipmap: more mover bricks than a fade's slot names");
			const auto isMoverBrick = [this](const MoverFade& fade)
			{
				return std::binary_search(m_MoverKeys.begin(), m_MoverKeys.end(), GetMoverKey(fade.Level, fade.Brick));
			};
			std::vector<bool> used(slots, false);
			uint32_t reserved = 0;
			uint32_t bricks = 0;
			const auto count = [&]()
			{
				std::fill(used.begin(), used.end(), false);
				reserved = 0;
				bricks = static_cast<uint32_t>(m_MoverKeys.size());
				for (const MoverFade& fade : m_Fades)
				{
					used[fade.Slot] = true;
					reserved = std::max(reserved, fade.Slot + 1);
					bricks += isMoverBrick(fade) ? 0u : 1u;
				}
			};
			count();
			// The movers come first: where their bricks need the slots, fades end at once, the last slot's first.
			while (bricks + reserved > slots)
			{
				m_Fades.erase(std::max_element(m_Fades.begin(), m_Fades.end(), [](const MoverFade& a, const MoverFade& b) { return a.Slot < b.Slot; }));
				count();
			}

			// The frame's events. Per level the voxels the bounds' triangles touch and one beyond (the box's soft edge reaches
			// there), by brick: a brick that fades already starts over from what it shows now, with both events' voxels.
			struct Start
			{
				MoverFade Fade;
				size_t Existing = c_New;
			};
			std::vector<Start> starts;
			std::vector<bool> trial;
			for (const AABB& bounds : m_FadeBounds)
			{
				starts.clear();
				for (uint32_t level = 0; level < m_Levels.size(); level++)
				{
					const VoxelLevelLayout& layout = m_Layout.Levels[level];
					VoxelBox touched = GetTouchedVoxels(bounds, layout.VoxelSize);
					if (touched.IsEmpty())
						continue;
					touched.Minimum -= int64_t(1);
					touched.Maximum += int64_t(1);
					const VoxelBox box = touched.Intersection(GlobalIllumination::GetWindow(layout, m_Levels[level].State.Window));
					if (box.IsEmpty())
						continue;
					const glm::i64vec3 first = GetMoverBrick(box.Minimum);
					const glm::i64vec3 last = GetMoverBrick(box.Maximum - int64_t(1));
					for (int64_t z = first.z; z <= last.z; z++)
					{
						for (int64_t y = first.y; y <= last.y; y++)
						{
							for (int64_t x = first.x; x <= last.x; x++)
							{
								Start start;
								start.Fade.Level = level;
								start.Fade.Brick = glm::i64vec3(x, y, z);
								const VoxelBox whole { start.Fade.Brick * c_Brick, start.Fade.Brick * c_Brick + c_Brick };
								const VoxelBox part = whole.Intersection(box);
								start.Fade.Low = glm::ivec3(part.Minimum - whole.Minimum);
								start.Fade.High = glm::ivec3(part.Maximum - whole.Minimum) - 1;
								for (size_t index = 0; index < m_Fades.size(); index++)
								{
									if (m_Fades[index].Level == level && m_Fades[index].Brick == start.Fade.Brick)
									{
										start.Existing = index;
										start.Fade.Low = glm::min(start.Fade.Low, m_Fades[index].Low);
										start.Fade.High = glm::max(start.Fade.High, m_Fades[index].High);
										break;
									}
								}
								// Only voxels the textures held: where the level was not valid (a slab that scrolled in waits, a
								// window is being rebuilt) nothing was shown, and the voxels change as they did.
								const VoxelBox faded { whole.Minimum + glm::i64vec3(start.Fade.Low), whole.Minimum + glm::i64vec3(start.Fade.High) + int64_t(1) };
								if (m_FadeValid[level].Contains(faded))
									starts.push_back(start);
							}
						}
					}
				}
				// An event fades whole or not at all (past the slots it changes in a step, as a mover past the layer's bricks is
				// in no voxel), and never takes a slot the bricks of the frame before still hold: they are put back after the
				// fades' are taken.
				trial = used;
				uint32_t trialReserved = reserved;
				uint32_t trialBricks = bricks;
				bool fits = true;
				for (Start& start : starts)
				{
					if (start.Existing != c_New)
						continue;
					const auto free = std::find(trial.begin(), trial.end(), false);
					if (free == trial.end())
					{
						fits = false;
						break;
					}
					*free = true;
					start.Fade.Slot = static_cast<uint32_t>(free - trial.begin());
					trialReserved = std::max(trialReserved, start.Fade.Slot + 1);
					trialBricks += isMoverBrick(start.Fade) ? 0u : 1u;
				}
				if (!fits || trialBricks + trialReserved > slots || trialReserved + m_MoverBrickCount > slots)
					continue;
				used = trial;
				reserved = trialReserved;
				bricks = trialBricks;
				for (const Start& start : starts)
				{
					if (start.Existing == c_New)
						m_Fades.push_back(start.Fade);
					MoverFade& fade = start.Existing == c_New ? m_Fades.back() : m_Fades[start.Existing];
					fade.Low = start.Fade.Low;
					fade.High = start.Fade.High;
					fade.Seconds = 0.0f;
					fade.Snapshot = true;
				}
			}

			// The frame's bricks: the movers' and those that only fade, in order.
			const std::ptrdiff_t held = static_cast<std::ptrdiff_t>(m_MoverKeys.size());
			for (const MoverFade& fade : m_Fades)
			{
				const uint64_t key = GetMoverKey(fade.Level, fade.Brick);
				if (!std::binary_search(m_MoverKeys.begin(), m_MoverKeys.begin() + held, key))
					m_MoverKeys.push_back(key);
				outFrame.MoverSnapshots += fade.Snapshot ? 1u : 0u;
			}
			std::sort(m_MoverKeys.begin() + held, m_MoverKeys.end());
			std::inplace_merge(m_MoverKeys.begin(), m_MoverKeys.begin() + held, m_MoverKeys.end());
			outFrame.MoverFades = static_cast<uint32_t>(m_Fades.size());
			ST_CORE_ASSERT(m_MoverKeys.size() + reserved <= slots, "VoxelClipmap: the fades' slots reach into the frame's bricks'");
		}
		m_FadeBounds.clear();
		m_MoverBrickCount = static_cast<uint32_t>(m_MoverKeys.size());
	}

	void VoxelClipmap::EmitMovers(const RenderScene& renderScene, const IVoxelSceneSource& source, const glm::dvec3& cameraPosition, float stepSeconds, VoxelFrame& outFrame)
	{
		constexpr int64_t c_Brick = RenderData::c_GIMoverBrickVoxels;
		m_MoverKeys.clear();
		m_MoverReach.clear();
		if (!m_Moving.empty() || !m_Resting.empty())
		{
			// Both lists are sorted and hold each proxy once; a proxy is in one of them at most.
			std::vector<uint32_t> movers;
			movers.reserve(m_Moving.size() + m_Resting.size());
			for (uint32_t proxy : m_Moving)
			{
				if (m_Proxies[proxy].Status == ProxyStatus::Moving)
					movers.push_back(proxy);
			}
			movers.insert(movers.end(), m_Resting.begin(), m_Resting.end());
			std::sort(movers.begin(), movers.end());
			outFrame.MovingProxies = static_cast<uint32_t>(movers.size());

			for (uint32_t proxy : movers)
			{
				// A proxy enters whole or not at all: past the boxes or the bricks it is left out (in no voxel until it rests).
				const size_t firstBox = outFrame.Movers.size();
				const size_t firstReach = m_MoverReach.size();
				bool fits = IsReady(renderScene, source, proxy);
				const uint32_t meshSlot = renderScene.GetMeshSlot(proxy);
				if (fits)
				{
					const MeshInfo& mesh = m_Meshes[meshSlot];
					const std::span<const uint32_t> submeshMaterials = renderScene.GetSubmeshMaterialSlots(meshSlot);
					const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
					const glm::mat4 world = renderScene.GetWorldTransform(proxy);
					const size_t drawn = std::min<size_t>(mesh.Submeshes.size(), renderScene.GetInstanceCount(proxy));
					for (size_t submesh = 0; submesh < drawn && fits; submesh++)
					{
						const uint32_t slot = overrideSlot != RenderScene::c_InvalidIndex ? overrideSlot
							: (submesh < submeshMaterials.size() ? submeshMaterials[submesh] : RenderScene::c_InvalidIndex);
						if (slot >= m_Materials.size())
							continue;
						const RenderData::GIMaterial& material = m_Materials[slot];
						const AABB& bounds = mesh.Submeshes[submesh].Bounds;
						if ((material.Flags & RenderData::c_CullMaterialAvailable) == 0 || (material.Flags & RenderData::c_CullMaterialTransparent) != 0 || !bounds.IsValid())
							continue;
						if (outFrame.Movers.size() >= RenderData::c_GIMaxMoverBoxes)
						{
							fits = false;
							break;
						}
						// The submesh's bounds under the proxy's world transform (the one it is drawn with): an oriented box.
						const glm::vec3 half = 0.5f * (bounds.Max - bounds.Min);
						const glm::dvec3 center(glm::vec3(world * glm::vec4(0.5f * (bounds.Min + bounds.Max), 1.0f)));
						const glm::vec3 axes[3] = { glm::vec3(world[0]) * half.x, glm::vec3(world[1]) * half.y, glm::vec3(world[2]) * half.z };
						RenderData::VoxelMover mover = {};
						mover.Center = glm::vec4(glm::vec3(center - cameraPosition), 0.0f);
						mover.AxisX = glm::vec4(axes[0], 0.0f);
						mover.AxisY = glm::vec4(axes[1], 0.0f);
						mover.AxisZ = glm::vec4(axes[2], 0.0f);
						mover.Albedo = material.Albedo;
						mover.Emission = material.Emission;
						mover.Flags = ((material.Flags & RenderData::c_CullMaterialAlphaMask) != 0 ? RenderData::c_VoxelMoverAlphaMasked : 0u)
							| ((material.Flags & RenderData::c_CullMaterialDoubleSided) != 0 ? RenderData::c_VoxelMoverDoubleSided : 0u);
						const uint32_t box = static_cast<uint32_t>(outFrame.Movers.size());
						outFrame.Movers.push_back(mover);
						m_LargestAlbedo = std::max({ m_LargestAlbedo, material.Albedo & 255u, (material.Albedo >> 8) & 255u, (material.Albedo >> 16) & 255u });

						// The bricks its voxels lie in: per level, every voxel of the window whose center the box reaches as
						// GI/VoxelMovers.comp weighs it (along each of its axes the box and a voxel beyond, at least two voxels each way).
						glm::vec3 units[3];
						bool flat = false;
						for (int axis = 0; axis < 3; axis++)
						{
							const float length = glm::length(axes[axis]);
							units[axis] = length > 1e-6f ? axes[axis] / length : glm::cross(axes[(axis + 1) % 3], axes[(axis + 2) % 3]);
							const float unitLength = glm::length(units[axis]);
							flat = flat || unitLength < 1e-12f;
							units[axis] = unitLength < 1e-12f ? glm::vec3(0.0f) : units[axis] / unitLength;
						}
						for (uint32_t level = 0; level < m_Levels.size() && !flat; level++)
						{
							const VoxelLevelLayout& layout = m_Layout.Levels[level];
							const double voxel = static_cast<double>(layout.VoxelSize);
							const VoxelBox window = GlobalIllumination::GetWindow(layout, m_Levels[level].State.Window);
							glm::dvec3 extent(0.0);
							for (int axis = 0; axis < 3; axis++)
							{
								const float width = layout.VoxelSize * (std::abs(units[axis].x) + std::abs(units[axis].y) + std::abs(units[axis].z));
								extent += glm::dvec3(glm::abs(units[axis])) * static_cast<double>(std::max(glm::length(axes[axis]), width) + width);
							}
							const glm::i64vec3 low = glm::max(glm::i64vec3(glm::floor((center - extent) / voxel)), window.Minimum);
							const glm::i64vec3 high = glm::min(glm::i64vec3(glm::floor((center + extent) / voxel)), window.Maximum - int64_t(1));
							if (low.x > high.x || low.y > high.y || low.z > high.z)
								continue;
							const glm::i64vec3 origin = GetMoverBrick(window.Minimum);
							const glm::i64vec3 first = GetMoverBrick(low) - origin;
							const glm::i64vec3 last = GetMoverBrick(high) - origin;
							for (int64_t z = first.z; z <= last.z; z++)
							{
								for (int64_t y = first.y; y <= last.y; y++)
								{
									for (int64_t x = first.x; x <= last.x; x++)
									{
										const uint64_t key = (static_cast<uint64_t>(level) << 48) | (static_cast<uint64_t>(z) << 32) | (static_cast<uint64_t>(y) << 16)
											| static_cast<uint64_t>(x);
										m_MoverReach.emplace_back(key, box);
									}
								}
							}
						}
					}
				}
				if (fits)
				{
					m_MoverProxyKeys.clear();
					for (size_t index = firstReach; index < m_MoverReach.size(); index++)
						m_MoverProxyKeys.push_back(m_MoverReach[index].first);
					std::sort(m_MoverProxyKeys.begin(), m_MoverProxyKeys.end());
					m_MoverProxyKeys.erase(std::unique(m_MoverProxyKeys.begin(), m_MoverProxyKeys.end()), m_MoverProxyKeys.end());
					const size_t held = m_MoverKeys.size();
					for (uint64_t key : m_MoverProxyKeys)
					{
						if (!std::binary_search(m_MoverKeys.begin(), m_MoverKeys.begin() + static_cast<std::ptrdiff_t>(held), key))
							m_MoverKeys.push_back(key);
					}
					fits = m_MoverKeys.size() <= m_Layout.MoverBricks;
					if (fits)
						std::inplace_merge(m_MoverKeys.begin(), m_MoverKeys.begin() + static_cast<std::ptrdiff_t>(held), m_MoverKeys.end());
					else
						m_MoverKeys.resize(held);
				}
				if (!fits)
				{
					outFrame.Movers.resize(firstBox);
					m_MoverReach.resize(firstReach);
					outFrame.MoversLeftOut++;
				}
			}

		}
		FollowFades(stepSeconds, outFrame);

		const glm::uvec3 dimensions = m_Layout.Levels.front().Dimensions;
		outFrame.MoverBricks.reserve(m_MoverKeys.size());
		for (size_t index = 0; index < m_MoverKeys.size(); index++)
		{
			const uint64_t key = m_MoverKeys[index];
			const uint32_t level = static_cast<uint32_t>(key >> 48);
			const glm::i64vec3 window = m_Levels[level].State.Window;
			const glm::i64vec3 brick = GetMoverBrick(window) + glm::i64vec3(key & 0xFFFFu, (key >> 16) & 0xFFFFu, (key >> 32) & 0xFFFFu);
			const glm::i64vec3 voxel = brick * c_Brick;
			RenderData::VoxelMoverBrick record = {};
			record.Local = glm::ivec4(glm::ivec3(voxel - window), static_cast<int32_t>(index));
			record.Texel = glm::ivec4(glm::ivec3(GlobalIllumination::GetTexel(voxel, dimensions)), static_cast<int32_t>(level));
			outFrame.MoverBricks.push_back(record);
		}
		for (const std::pair<uint64_t, uint32_t>& reach : m_MoverReach)
		{
			const size_t index = static_cast<size_t>(std::lower_bound(m_MoverKeys.begin(), m_MoverKeys.end(), reach.first) - m_MoverKeys.begin());
			outFrame.MoverBricks[index].Movers[static_cast<int>(reach.second / 32)] |= 1u << (reach.second % 32);
		}
		for (const MoverFade& fade : m_Fades)
		{
			const size_t index = static_cast<size_t>(std::lower_bound(m_MoverKeys.begin(), m_MoverKeys.end(), GetMoverKey(fade.Level, fade.Brick)) - m_MoverKeys.begin());
			const float time = std::clamp(fade.Seconds / RenderData::c_GIMoverFadeSeconds, 0.0f, 1.0f);
			const glm::uvec3 low(fade.Low);
			const glm::uvec3 high(fade.High);
			glm::uvec4& movers = outFrame.MoverBricks[index].Movers;
			movers.z = (m_Layout.MoverBricks - fade.Slot) | (low.x << RenderData::c_VoxelMoverFadeLowShift) | (low.y << (RenderData::c_VoxelMoverFadeLowShift + 3))
				| (low.z << (RenderData::c_VoxelMoverFadeLowShift + 6)) | (high.x << RenderData::c_VoxelMoverFadeHighShift)
				| (high.y << (RenderData::c_VoxelMoverFadeHighShift + 3)) | (high.z << (RenderData::c_VoxelMoverFadeHighShift + 6))
				| (fade.Snapshot ? RenderData::c_VoxelMoverFadeSnapshot : 0u);
			movers.w = glm::floatBitsToUint(time);
		}

		uint64_t hash = HashBytes(outFrame.Movers.data(), outFrame.Movers.size() * sizeof(RenderData::VoxelMover));
		hash = HashBytes(outFrame.MoverBricks.data(), outFrame.MoverBricks.size() * sizeof(RenderData::VoxelMoverBrick), hash);
		outFrame.MoversChanged = hash != m_MoverHash;
		m_MoverHash = hash;
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
			const auto slotOf = [&](size_t submesh) -> uint32_t
			{
				return overrideSlot != RenderScene::c_InvalidIndex ? overrideSlot : (submesh < submeshMaterials.size() ? submeshMaterials[submesh] : RenderScene::c_InvalidIndex);
			};
			const auto materialOf = [&](size_t submesh) -> uint32_t
			{
				const uint32_t slot = slotOf(submesh);
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
				const uint32_t albedo = m_Materials[slotOf(submesh)].Albedo; // Available: the slot is in the table
				m_LargestAlbedo = std::max({ m_LargestAlbedo, albedo & 255u, (albedo >> 8) & 255u, (albedo >> 16) & 255u });
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
