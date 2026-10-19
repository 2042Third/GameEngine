#include "stpch.h"
#include "Strata/Renderer/RenderSceneGPU.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/Assert.h"
#include "Strata/Core/ParallelClone.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/GeometryPool.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/MaterialData.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Renderer/TextureTable.h"

#include <algorithm>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_NoOwner = ~0u;
		constexpr uint32_t c_NotListed = ~0u;
		static_assert(RenderProxyFlags::Skinned < (1u << RenderData::c_GPUProxyOverrideShift), "The proxy flags must fit below the override slot");
		// Changed instances (or records) this close together are uploaded in one run: fewer copies for a little more data.
		constexpr uint32_t c_InstanceRunGap = 16;
		constexpr uint32_t c_RecordRunGap = 16;
		constexpr uint32_t c_TableRunGap = 16;
		// A material slot's textures before it names any (AssetHandle's default constructor makes a random handle).
		constexpr std::array<AssetHandle, 5> c_NoTextures = { AssetHandle::Null(), AssetHandle::Null(), AssetHandle::Null(), AssetHandle::Null(),
			AssetHandle::Null() };

		// Buffer versions are unique in the process (main thread only), so a mirror made again for a scene never repeats one a
		// renderer's binding sets were made for.
		uint64_t s_LastBufferVersion = 0;

	}

	std::mutex RenderSceneGPUCache::s_Mutex;
	std::unordered_set<uint64_t> RenderSceneGPUCache::s_Mirrored;
	std::vector<uint64_t> RenderSceneGPUCache::s_Destroyed;

	RenderSceneGPU::RenderSceneGPU(nvrhi::IDevice* device, uint64_t renderSceneId)
		: m_Device(device), m_RenderSceneId(renderSceneId)
	{
		m_CommandList = m_Device->createCommandList();
	}

	RenderSceneGPU::~RenderSceneGPU() = default;

	Scope<RenderSceneGPU> RenderSceneGPU::CreateClone(RenderSceneGPU& source, uint64_t renderSceneId)
	{
		ST_PROFILE_FUNCTION();
		// Member-wise, the per-proxy records and states copied in parallel (a large scene's bytes).
		Scope<RenderSceneGPU> clone = CloneWithParallelMembers(source, [&source]() { return Scope<RenderSceneGPU>(new RenderSceneGPU(source)); },
			&RenderSceneGPU::m_Records, &RenderSceneGPU::m_Resident, &RenderSceneGPU::m_Queued, &RenderSceneGPU::m_Seen, &RenderSceneGPU::m_InstanceOwners);
		clone->m_RenderSceneId = renderSceneId;
		clone->m_CommandList = clone->m_Device->createCommandList();
		if (!clone->m_CommandList)
			return nullptr;
		clone->m_CommandList->open();
		bool created = true;
		for (GrowingBuffer* buffer : { &clone->m_Instances, &clone->m_Proxies, &clone->m_Palette, &clone->m_MeshTableBuffer, &clone->m_MaterialBuffer,
			&clone->m_MaterialFlagBuffer, &clone->m_LightRecords })
		{
			if (!buffer->Buffer)
				continue;
			// The source's buffer, copied on the GPU (frames in flight only read it).
			nvrhi::BufferDesc desc = buffer->Buffer->getDesc();
			desc.debugName += ".Clone";
			nvrhi::BufferHandle copy = clone->m_Device->createBuffer(desc);
			if (!copy)
			{
				ST_CORE_ERROR("RenderSceneGPU: failed to create the clone of {} ({} bytes)", buffer->Buffer->getDesc().debugName, desc.byteSize);
				created = false;
				break;
			}
			clone->m_CommandList->copyBuffer(copy, 0, buffer->Buffer, 0, desc.byteSize);
			clone->m_CommandList->setBufferState(copy, nvrhi::ResourceStates::ShaderResource);
			buffer->Buffer = std::move(copy);
		}
		clone->m_CommandList->commitBarriers();
		clone->m_CommandList->close();
		clone->m_Device->executeCommandList(clone->m_CommandList);
		if (!created)
			return nullptr;
		clone->m_BufferVersion = ++s_LastBufferVersion;
		return clone;
	}

	bool RenderSceneGPU::HoldsVersions(const RenderScene::ChangeVersions& versions) const
	{
		return m_Initialized && !m_UploadAllTables && m_ProxyVersion == versions.Proxy && m_InstanceVersion == versions.Instance
			&& m_PaletteVersion == versions.Palette && m_LightVersion == versions.Light;
	}

	uint64_t RenderSceneGPU::GetBufferBytes() const
	{
		uint64_t bytes = 0;
		for (const GrowingBuffer* buffer : { &m_Instances, &m_Proxies, &m_Palette, &m_MeshTableBuffer, &m_MaterialBuffer, &m_MaterialFlagBuffer, &m_LightRecords })
			bytes += buffer->Buffer ? buffer->Buffer->getDesc().byteSize : 0;
		return bytes;
	}

	void RenderSceneGPU::BeginFrame(uint64_t frame)
	{
		m_Frame = frame;
	}

	RenderData::GPUProxy RenderSceneGPU::MakeRecord(const RenderScene& renderScene, uint32_t proxy)
	{
		RenderData::GPUProxy record;
		if (renderScene.GetEntity(proxy) == entt::null)
			return record;
		const AABB& local = renderScene.GetLocalBounds(proxy);
		const AABB& world = renderScene.GetWorldBounds(proxy);
		uint32_t flags = renderScene.GetProxyFlags(proxy);
		if (local.IsValid() && world.IsValid())
		{
			record.LocalMin = local.Min;
			record.LocalMax = local.Max;
			record.WorldMin = world.Min;
			record.WorldMax = world.Max;
		}
		else
		{
			// No bounds to cull with: never in a view (the spatial index does not hold it either).
			flags &= ~RenderProxyFlags::Indexed;
		}
		record.FirstInstance = renderScene.GetFirstInstance(proxy);
		record.InstanceCount = renderScene.GetInstanceCount(proxy);
		record.MeshSlot = renderScene.GetMeshSlot(proxy);
		const uint32_t overrideSlot = renderScene.GetOverrideMaterialSlot(proxy);
		if (overrideSlot <= RenderData::c_GPUProxyMaxOverrideSlot)
			flags |= (overrideSlot + 1) << RenderData::c_GPUProxyOverrideShift;
		record.Flags = flags;
		return record;
	}

	bool RenderSceneGPU::EnsureCapacity(GrowingBuffer& buffer, uint64_t elementCount, uint32_t elementSize, const char* name, bool clear)
	{
		const uint64_t required = std::max<uint64_t>(elementCount, 1);
		if (buffer.Buffer && buffer.ElementCount >= required)
			return true;
		// Geometric growth, so a scene that grows a little at a time does not copy every frame.
		uint64_t capacity = std::max<uint64_t>(buffer.ElementCount, 64);
		while (capacity < required)
			capacity *= 2;

		nvrhi::BufferDesc desc;
		desc.byteSize = capacity * elementSize;
		desc.structStride = elementSize;
		desc.debugName = fmt::format("RenderSceneGPU.{}", name);
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		nvrhi::BufferHandle created = m_Device->createBuffer(desc);
		if (!created)
		{
			ST_CORE_ERROR("RenderSceneGPU: failed to create its {} buffer ({} bytes)", name, desc.byteSize);
			return false;
		}
		// The clear, the copy and the update's uploads write the buffer one after the other: NVRHI places no barrier between
		// transfers into a buffer that stays a copy destination, so each step ends with a transition of its own.
		const auto barrier = [this, &created]()
		{
			m_CommandList->setBufferState(created, nvrhi::ResourceStates::ShaderResource);
			m_CommandList->commitBarriers();
		};
		if (clear)
		{
			m_CommandList->clearBufferUInt(created, 0);
			barrier();
		}
		if (buffer.Buffer)
		{
			// What it holds moves over on the GPU; frames in flight may still read the old buffer.
			m_CommandList->copyBuffer(created, 0, buffer.Buffer, 0, buffer.ElementCount * elementSize);
			barrier();
			Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(buffer.Buffer.Get()) });
		}
		buffer.Buffer = std::move(created);
		buffer.ElementCount = capacity;
		m_BufferVersion = ++s_LastBufferVersion;
		return true;
	}

	void RenderSceneGPU::MarkRecord(uint32_t proxy)
	{
		m_RecordMarks.push_back(proxy);
	}

	void RenderSceneGPU::MarkInstances(uint32_t first, uint32_t count)
	{
		for (uint32_t index = 0; index < count; index++)
			m_InstanceMarks.push_back(first + index);
	}

	void RenderSceneGPU::AddRuns(std::vector<uint32_t>& marks, nvrhi::IBuffer* buffer, const uint8_t* data, uint32_t elementSize, uint32_t gap, uint32_t* outRuns)
	{
		if (marks.empty())
			return;
		std::sort(marks.begin(), marks.end());
		marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
		size_t start = 0;
		for (size_t index = 1; index <= marks.size(); index++)
		{
			if (index < marks.size() && marks[index] <= marks[index - 1] + gap)
				continue;
			const uint64_t first = marks[start];
			const uint64_t end = static_cast<uint64_t>(marks[index - 1]) + 1;
			m_Regions.push_back(GPUBufferRegion { buffer, first * elementSize, data + first * elementSize, (end - first) * elementSize });
			if (outRuns)
				(*outRuns)++;
			start = index;
		}
	}

	void RenderSceneGPU::SetMaterialFlags(uint32_t slot, uint32_t flags)
	{
		const uint32_t previous = m_MaterialFlags[slot];
		if ((previous & RenderData::c_CullMaterialDoubleSided) != 0)
			m_DoubleSidedMaterials--;
		if ((previous & RenderData::c_CullMaterialAlphaMask) != 0)
			m_AlphaMaskedMaterials--;
		if ((flags & RenderData::c_CullMaterialDoubleSided) != 0)
			m_DoubleSidedMaterials++;
		if ((flags & RenderData::c_CullMaterialAlphaMask) != 0)
			m_AlphaMaskedMaterials++;
		const bool wasTransparent = (previous & RenderData::c_CullMaterialTransparent) != 0;
		const bool transparent = (flags & RenderData::c_CullMaterialTransparent) != 0;
		if (transparent && !wasTransparent)
		{
			m_TransparentPositions[slot] = static_cast<uint32_t>(m_TransparentSlots.size());
			m_TransparentSlots.push_back(slot);
		}
		else if (!transparent && wasTransparent)
		{
			const uint32_t position = m_TransparentPositions[slot];
			const uint32_t moved = m_TransparentSlots.back();
			m_TransparentSlots[position] = moved;
			m_TransparentPositions[moved] = position;
			m_TransparentSlots.pop_back();
			m_TransparentPositions[slot] = c_NotListed;
		}
		m_MaterialFlags[slot] = flags;
	}

	void RenderSceneGPU::RemoveTextureUsers(uint32_t slot)
	{
		std::array<AssetHandle, 5>& textures = m_MaterialTextures[slot];
		for (size_t map = 0; map < textures.size(); map++)
		{
			const AssetHandle texture = textures[map];
			if (!texture.IsValid())
				continue;
			textures[map] = AssetHandle::Null();
			const auto found = m_TextureUsers.find(texture);
			ST_CORE_ASSERT(found != m_TextureUsers.end(), "Material slot {} names a texture without users", slot);
			if (found == m_TextureUsers.end())
				continue;
			// The list's last user takes the slot's place.
			std::vector<uint32_t>& users = found->second;
			const uint32_t position = m_MaterialTexturePositions[slot][map];
			const uint32_t moved = users.back();
			users[position] = moved;
			users.pop_back();
			if (moved != slot)
			{
				for (size_t other = 0; other < m_MaterialTextures[moved].size(); other++)
				{
					if (m_MaterialTextures[moved][other] == texture)
					{
						m_MaterialTexturePositions[moved][other] = position;
						break;
					}
				}
			}
			if (users.empty())
				m_TextureUsers.erase(found);
		}
	}

	void RenderSceneGPU::BuildMeshEntry(uint32_t slot, AssetHandle handle, const AssetManagerBase* assets)
	{
		if (m_MeshRecords[slot] > 0)
			m_ArenaRecords[(m_MeshTable[slot] & RenderData::c_CullMeshSkinnedArena) != 0 ? 1 : 0] -= m_MeshRecords[slot];
		m_MeshSlotHandles[slot] = handle;
		m_MeshTable[slot] = 0;
		m_MeshRecords[slot] = 0;
		const Ref<Mesh> mesh = assets ? assets->PeekLoadedAsset<Mesh>(handle) : nullptr;
		const GeometryAllocation* geometry = mesh ? mesh->GetGeometry() : nullptr;
		if (!geometry)
			return; // Not loaded, or its geometry not in the pool yet: nothing to draw
		const GeometryHandle geometryHandle = mesh->GetGeometryHandle();
		ST_CORE_ASSERT(geometryHandle != c_InvalidGeometry && geometryHandle < RenderData::c_CullMeshSkinnedArena, "Geometry handle {} does not fit the mesh table",
			geometryHandle);
		uint32_t records = 0;
		for (const Submesh& submesh : mesh->GetSubmeshes())
			records += static_cast<uint32_t>(submesh.LODs.size());
		const bool skinnedArena = geometry->Arena == GeometryArena::Skinned;
		m_MeshTable[slot] = geometryHandle | (skinnedArena ? RenderData::c_CullMeshSkinnedArena : 0u);
		m_MeshRecords[slot] = records;
		m_ArenaRecords[skinnedArena ? 1 : 0] += records;
	}

	void RenderSceneGPU::BuildMaterial(uint32_t slot, AssetHandle handle, const AssetManagerBase* assets)
	{
		RemoveTextureUsers(slot);
		m_MaterialSlotHandles[slot] = handle;
		m_MaterialUsesDefault[slot] = 0;
		RenderData::MaterialData data = {};
		uint32_t flags = 0;
		if (handle.IsValid() && assets)
		{
			// The default material stands in while the material loads (or when it never will).
			Ref<Material> material = assets->PeekLoadedAsset<Material>(handle);
			if (!material)
			{
				m_MaterialUsesDefault[slot] = 1;
				material = assets->PeekLoadedAsset<Material>(BuiltinAssets::DefaultMaterial);
			}
			if (material)
			{
				data = MaterialGPU::Build(*material, [assets](AssetHandle texture, uint32_t missingIndex, uint32_t loadingIndex)
				{
					if (!texture.IsValid())
						return missingIndex;
					const Ref<Texture> loaded = assets->PeekLoadedAsset<Texture>(texture);
					return loaded && loaded->GetTableIndex() != TextureTable::c_InvalidIndex ? loaded->GetTableIndex() : loadingIndex;
				});
				flags = MaterialGPU::GetFlags(material->GetProperties());

				// Its textures, each once, whether loaded or not: their loads and evictions build the slot again.
				const std::array<AssetHandle, MaterialGPU::c_TextureMapCount> maps = MaterialGPU::GetTextureMaps(material->GetProperties());
				if (std::any_of(maps.begin(), maps.end(), [](AssetHandle map) { return map.IsValid(); }))
					flags |= RenderData::c_CullMaterialTextured;
				std::array<AssetHandle, 5>& textures = m_MaterialTextures[slot];
				for (size_t map = 0; map < maps.size(); map++)
				{
					const AssetHandle texture = maps[map];
					if (!texture.IsValid() || std::find(maps.begin(), maps.begin() + static_cast<ptrdiff_t>(map), texture) != maps.begin() + static_cast<ptrdiff_t>(map))
						continue;
					std::vector<uint32_t>& users = m_TextureUsers[texture];
					textures[map] = texture;
					m_MaterialTexturePositions[slot][map] = static_cast<uint32_t>(users.size());
					users.push_back(slot);
				}
			}
		}
		m_MaterialData[slot] = data;
		SetMaterialFlags(slot, flags);
	}

	bool RenderSceneGPU::AssetTablesChanged(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets) const
	{
		if (m_UploadAllTables || renderScene.GetSlotsVersion() != m_SlotsVersion || renderScene.GetMeshSlotCapacity() != m_MeshTable.size()
			|| renderScene.GetMaterialSlotCapacity() != m_MaterialFlags.size())
			return true;
		if (m_HasAssets != (assets != nullptr) || m_Assets.lock() != assets)
			return true;
		return assets && assets->GetContentVersion() != m_AssetContentVersion;
	}

	void RenderSceneGPU::UpdateAssetTables(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets, RenderSceneGPUUpdate& update)
	{
		ST_PROFILE_FUNCTION();
		const AssetManagerBase* manager = assets.get();
		m_MeshMarks.clear();
		m_MaterialMarks.clear();

		// Slots the scene no longer has (render scenes only grow theirs) leave the counts first.
		const uint32_t meshSlots = renderScene.GetMeshSlotCapacity();
		const uint32_t materialSlots = renderScene.GetMaterialSlotCapacity();
		for (uint32_t slot = meshSlots; slot < m_MeshTable.size(); slot++)
			BuildMeshEntry(slot, AssetHandle::Null(), nullptr);
		for (uint32_t slot = materialSlots; slot < m_MaterialFlags.size(); slot++)
			BuildMaterial(slot, AssetHandle::Null(), nullptr);
		m_MeshSlotHandles.resize(meshSlots, AssetHandle::Null());
		m_MeshTable.resize(meshSlots, 0u);
		m_MeshRecords.resize(meshSlots, 0u);
		m_MaterialSlotHandles.resize(materialSlots, AssetHandle::Null());
		m_MaterialData.resize(materialSlots, RenderData::MaterialData {});
		m_MaterialFlags.resize(materialSlots, 0u);
		m_MaterialUsesDefault.resize(materialSlots, 0);
		m_MaterialTextures.resize(materialSlots, c_NoTextures);
		m_MaterialTexturePositions.resize(materialSlots, std::array<uint32_t, 5> {});
		m_TransparentPositions.resize(materialSlots, c_NotListed);

		// Another manager (or none) means other objects for every handle; so does a manager that no longer remembers the
		// changes since the tables were built.
		bool rebuildAll = m_HasAssets != (assets != nullptr) || m_Assets.lock() != assets;
		const uint64_t contentVersion = manager ? manager->GetContentVersion() : 0;
		m_ContentChanges.clear();
		if (!rebuildAll && manager && contentVersion != m_AssetContentVersion && !manager->GetContentChanges(m_AssetContentVersion, m_ContentChanges))
			rebuildAll = true;
		m_Assets = assets;
		m_HasAssets = assets != nullptr;
		m_AssetContentVersion = contentVersion;

		if (rebuildAll)
		{
			for (uint32_t slot = 0; slot < meshSlots; slot++)
				m_MeshMarks.push_back(slot);
			for (uint32_t slot = 0; slot < materialSlots; slot++)
				m_MaterialMarks.push_back(slot);
		}
		else
		{
			// Slots taken by other assets.
			if (renderScene.GetSlotsVersion() != m_SlotsVersion)
			{
				for (uint32_t slot = 0; slot < meshSlots; slot++)
				{
					if (renderScene.GetMeshHandle(slot) != m_MeshSlotHandles[slot])
						m_MeshMarks.push_back(slot);
				}
				for (uint32_t slot = 0; slot < materialSlots; slot++)
				{
					if (renderScene.GetMaterialHandle(slot) != m_MaterialSlotHandles[slot])
						m_MaterialMarks.push_back(slot);
				}
			}
			// Assets loaded, reloaded or dropped: the slots of the meshes and materials, the materials naming the textures, and
			// those drawn as the default material when it is the one that changed.
			for (const AssetHandle handle : m_ContentChanges)
			{
				const uint32_t meshSlot = renderScene.FindMeshSlot(handle);
				if (meshSlot < meshSlots)
					m_MeshMarks.push_back(meshSlot);
				const uint32_t materialSlot = renderScene.FindMaterialSlot(handle);
				if (materialSlot < materialSlots)
					m_MaterialMarks.push_back(materialSlot);
				const auto users = m_TextureUsers.find(handle);
				if (users != m_TextureUsers.end())
					m_MaterialMarks.insert(m_MaterialMarks.end(), users->second.begin(), users->second.end());
				if (handle == BuiltinAssets::DefaultMaterial)
				{
					for (uint32_t slot = 0; slot < materialSlots; slot++)
					{
						if (m_MaterialUsesDefault[slot])
							m_MaterialMarks.push_back(slot);
					}
				}
			}
		}
		m_SlotsVersion = renderScene.GetSlotsVersion();

		// Each marked slot built once.
		std::sort(m_MeshMarks.begin(), m_MeshMarks.end());
		m_MeshMarks.erase(std::unique(m_MeshMarks.begin(), m_MeshMarks.end()), m_MeshMarks.end());
		std::sort(m_MaterialMarks.begin(), m_MaterialMarks.end());
		m_MaterialMarks.erase(std::unique(m_MaterialMarks.begin(), m_MaterialMarks.end()), m_MaterialMarks.end());
		for (const uint32_t slot : m_MeshMarks)
			BuildMeshEntry(slot, renderScene.GetMeshHandle(slot), manager);
		for (const uint32_t slot : m_MaterialMarks)
			BuildMaterial(slot, renderScene.GetMaterialHandle(slot), manager);
		update.MeshesChecked = static_cast<uint32_t>(m_MeshMarks.size());
		update.MaterialsBuilt = static_cast<uint32_t>(m_MaterialMarks.size());
		if (!m_MeshMarks.empty() || !m_MaterialMarks.empty())
			m_TablesVersion++;

		if (m_UploadAllTables)
		{
			// The previous upload of the tables failed: all of them again.
			m_MeshMarks.resize(meshSlots);
			for (uint32_t slot = 0; slot < meshSlots; slot++)
				m_MeshMarks[slot] = slot;
			m_MaterialMarks.resize(materialSlots);
			for (uint32_t slot = 0; slot < materialSlots; slot++)
				m_MaterialMarks[slot] = slot;
			m_UploadAllTables = false;
		}
	}

	bool RenderSceneGPU::Update(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets, uint64_t newProxyBytesPerFrame, RenderSceneGPUUpdate* outUpdate)
	{
		ST_PROFILE_FUNCTION();
		RenderSceneGPUUpdate update;
		m_LastUsedFrame = m_Frame;
		if (m_BudgetFrame != m_Frame)
		{
			m_BudgetFrame = m_Frame;
			m_BudgetLeft = newProxyBytesPerFrame;
			m_FrameNewProxies = 0;
		}
		const RenderLights& lights = renderScene.GetLights();
		const bool changed = !m_Initialized || renderScene.GetProxyVersion() != m_ProxyVersion || renderScene.GetInstanceVersion() != m_InstanceVersion
			|| renderScene.GetPaletteVersion() != m_PaletteVersion || lights.GetVersion() != m_LightVersion;
		const bool pendingRoom = m_PendingHead < m_Pending.size() && (m_BudgetLeft > 0 || m_FrameNewProxies == 0);
		const bool tablesChanged = AssetTablesChanged(renderScene, assets);
		if (!changed && !pendingRoom && !tablesChanged)
		{
			if (outUpdate)
				*outUpdate = update;
			return true;
		}

		const std::span<const RenderData::InstanceData> instances = renderScene.GetInstances();
		const std::span<const RenderData::AffineTransform> palette = renderScene.GetPalette();
		const uint32_t proxyCount = renderScene.GetProxyCapacity();
		m_CommandList->open();
		// The copies are the frame's uploads to the GPU's time per stage.
		GPUPassTimer gpuTimer(m_CommandList, RendererGPUStages::Get().Uploads);
		const bool buffersReady = EnsureCapacity(m_Proxies, proxyCount, sizeof(RenderData::GPUProxy), "Proxies", true)
			&& EnsureCapacity(m_Instances, instances.size(), sizeof(RenderData::InstanceData), "Instances", false)
			&& EnsureCapacity(m_Palette, palette.size(), sizeof(RenderData::AffineTransform), "Palette", false)
			&& EnsureCapacity(m_MeshTableBuffer, renderScene.GetMeshSlotCapacity(), sizeof(uint32_t), "MeshTable", true)
			&& EnsureCapacity(m_MaterialBuffer, renderScene.GetMaterialSlotCapacity(), sizeof(RenderData::MaterialData), "Materials", true)
			&& EnsureCapacity(m_MaterialFlagBuffer, renderScene.GetMaterialSlotCapacity(), sizeof(uint32_t), "MaterialFlags", true)
			&& EnsureCapacity(m_LightRecords, lights.GetCapacity(), sizeof(RenderData::LightRecord), "LightRecords", true);
		if (!buffersReady)
		{
			// The copies of the buffers made so far keep what they held.
			gpuTimer.End();
			m_CommandList->close();
			m_Device->executeCommandList(m_CommandList);
			if (outUpdate)
				*outUpdate = update;
			return false;
		}
		m_Records.resize(proxyCount);
		m_Resident.resize(proxyCount, 0);
		m_Queued.resize(proxyCount, 0);
		m_Seen.resize(proxyCount, 0);
		m_InstanceOwners.resize(instances.size(), c_NoOwner);
		if (++m_UpdateStamp == 0)
		{
			std::fill(m_Seen.begin(), m_Seen.end(), 0u);
			m_UpdateStamp = 1;
		}
		m_RecordMarks.clear();
		m_InstanceMarks.clear();
		m_Regions.clear();
		m_Promoted.clear();

		// Proxies: changes of resident ones upload now, destroyed ones are cleared, new ones wait for the budget.
		m_Changes.clear();
		const bool allProxies = !m_Initialized || !renderScene.GetProxyChanges(m_ProxyVersion, m_Changes);
		if (allProxies)
		{
			m_Changes.resize(proxyCount);
			for (uint32_t proxy = 0; proxy < proxyCount; proxy++)
				m_Changes[proxy] = proxy;
		}
		for (const uint32_t proxy : m_Changes)
		{
			if (proxy >= proxyCount || m_Seen[proxy] == m_UpdateStamp)
				continue;
			m_Seen[proxy] = m_UpdateStamp;
			if (renderScene.GetEntity(proxy) == entt::null)
			{
				// Its record is cleared at once; a proxy that takes its index later is a new one.
				m_Records[proxy] = RenderData::GPUProxy {};
				MarkRecord(proxy);
				m_Resident[proxy] = 0;
				continue;
			}
			const uint32_t first = renderScene.GetFirstInstance(proxy);
			const uint32_t count = renderScene.GetInstanceCount(proxy);
			for (uint32_t index = 0; index < count; index++)
				m_InstanceOwners[first + index] = proxy;
			if (m_Resident[proxy])
			{
				m_Records[proxy] = MakeRecord(renderScene, proxy);
				MarkRecord(proxy);
			}
			else if (!m_Queued[proxy])
			{
				m_Queued[proxy] = 1;
				m_Pending.push_back(proxy);
			}
		}

		// Instances of resident proxies (those of others upload with their proxies).
		m_Changes.clear();
		const bool allInstances = !m_Initialized || !renderScene.GetInstanceChanges(m_InstanceVersion, m_Changes);
		if (allInstances)
		{
			for (uint32_t proxy = 0; proxy < proxyCount; proxy++)
			{
				if (m_Resident[proxy])
					MarkInstances(renderScene.GetFirstInstance(proxy), renderScene.GetInstanceCount(proxy));
			}
		}
		else
		{
			for (const uint32_t instance : m_Changes)
			{
				if (instance < instances.size() && m_InstanceOwners[instance] != c_NoOwner && m_Resident[m_InstanceOwners[instance]])
					m_InstanceMarks.push_back(instance);
			}
		}

		// New proxies, oldest first, while the frame's budget lasts (and one each frame whatever its size).
		while (m_PendingHead < m_Pending.size())
		{
			const uint32_t proxy = m_Pending[m_PendingHead];
			if (proxy >= proxyCount || renderScene.GetEntity(proxy) == entt::null || m_Resident[proxy])
			{
				if (proxy < proxyCount)
					m_Queued[proxy] = 0;
				m_PendingHead++;
				continue;
			}
			const uint32_t count = renderScene.GetInstanceCount(proxy);
			const uint64_t bytes = sizeof(RenderData::GPUProxy) + static_cast<uint64_t>(count) * sizeof(RenderData::InstanceData);
			if (bytes > m_BudgetLeft && m_FrameNewProxies > 0)
				break;
			m_BudgetLeft -= std::min(m_BudgetLeft, bytes);
			m_FrameNewProxies++;
			m_Queued[proxy] = 0;
			m_Resident[proxy] = 1;
			m_Promoted.push_back(proxy);
			m_Records[proxy] = MakeRecord(renderScene, proxy);
			MarkRecord(proxy);
			MarkInstances(renderScene.GetFirstInstance(proxy), count);
			update.NewProxies++;
			m_PendingHead++;
		}
		if (m_PendingHead == m_Pending.size())
		{
			m_Pending.clear();
			m_PendingHead = 0;
		}
		else if (m_PendingHead > m_Pending.size() / 2)
		{
			m_Pending.erase(m_Pending.begin(), m_Pending.begin() + static_cast<ptrdiff_t>(m_PendingHead));
			m_PendingHead = 0;
		}
		m_DeferredBytes = 0;
		m_DeferredProxies = 0;
		for (size_t index = m_PendingHead; index < m_Pending.size(); index++)
		{
			const uint32_t proxy = m_Pending[index];
			if (proxy >= proxyCount || renderScene.GetEntity(proxy) == entt::null || m_Resident[proxy])
				continue;
			m_DeferredBytes += sizeof(RenderData::GPUProxy) + static_cast<uint64_t>(renderScene.GetInstanceCount(proxy)) * sizeof(RenderData::InstanceData);
			m_DeferredProxies++;
		}

		// Palettes: the joint ranges that changed.
		uint32_t paletteRuns = 0;
		if (!palette.empty() && (!m_Initialized || renderScene.GetPaletteVersion() != m_PaletteVersion))
		{
			m_PaletteRanges.clear();
			if (!m_Initialized || !renderScene.GetPaletteChanges(m_PaletteVersion, m_PaletteRanges))
			{
				m_PaletteRanges.clear();
				m_PaletteRanges.emplace_back(0u, static_cast<uint32_t>(palette.size()));
			}
			// Ranges are logged newest first and may repeat: each joint once, in merged runs.
			std::sort(m_PaletteRanges.begin(), m_PaletteRanges.end());
			uint32_t runStart = 0;
			uint32_t runEnd = 0;
			const auto flush = [&]()
			{
				if (runEnd <= runStart)
					return;
				const uint64_t bytes = static_cast<uint64_t>(runEnd - runStart) * sizeof(RenderData::AffineTransform);
				m_Regions.push_back(GPUBufferRegion { m_Palette.Buffer, runStart * sizeof(RenderData::AffineTransform),
					reinterpret_cast<const uint8_t*>(palette.data() + runStart), bytes });
				update.PaletteBytes += bytes;
				paletteRuns++;
			};
			for (const auto& [first, count] : m_PaletteRanges)
			{
				if (first > runEnd)
				{
					flush();
					runStart = first;
				}
				runEnd = std::min(std::max(runEnd, first + count), static_cast<uint32_t>(palette.size()));
			}
			flush();
		}

		// Light records: those that changed (all of them when the log does not reach back far enough).
		if (lights.GetCapacity() > 0 && (!m_Initialized || lights.GetVersion() != m_LightVersion))
		{
			m_LightMarks.clear();
			if (!m_Initialized || !lights.GetChanges(m_LightVersion, m_LightMarks))
			{
				m_LightMarks.resize(lights.GetCapacity());
				for (uint32_t record = 0; record < lights.GetCapacity(); record++)
					m_LightMarks[record] = record;
			}
			const size_t lightRegions = m_Regions.size();
			AddRuns(m_LightMarks, m_LightRecords.Buffer, reinterpret_cast<const uint8_t*>(lights.GetRecords().data()), sizeof(RenderData::LightRecord),
				c_RecordRunGap, nullptr);
			for (size_t index = lightRegions; index < m_Regions.size(); index++)
				update.LightBytes += m_Regions[index].Size;
		}

		// The asset tables: what the scene's mesh and material slots hold now.
		if (tablesChanged)
		{
			UpdateAssetTables(renderScene, assets, update);
			const size_t tableRegions = m_Regions.size();
			AddRuns(m_MeshMarks, m_MeshTableBuffer.Buffer, reinterpret_cast<const uint8_t*>(m_MeshTable.data()), sizeof(uint32_t), c_TableRunGap, nullptr);
			AddRuns(m_MaterialMarks, m_MaterialFlagBuffer.Buffer, reinterpret_cast<const uint8_t*>(m_MaterialFlags.data()), sizeof(uint32_t), c_TableRunGap, nullptr);
			const size_t materialRegions = m_Regions.size();
			AddRuns(m_MaterialMarks, m_MaterialBuffer.Buffer, reinterpret_cast<const uint8_t*>(m_MaterialData.data()), sizeof(RenderData::MaterialData), c_TableRunGap,
				nullptr);
			for (size_t index = tableRegions; index < m_Regions.size(); index++)
				(index < materialRegions ? update.TableBytes : update.MaterialBytes) += m_Regions[index].Size;
		}

		const size_t recordRegions = m_Regions.size();
		AddRuns(m_RecordMarks, m_Proxies.Buffer, reinterpret_cast<const uint8_t*>(m_Records.data()), sizeof(RenderData::GPUProxy), c_RecordRunGap, nullptr);
		for (size_t index = recordRegions; index < m_Regions.size(); index++)
			update.ProxyBytes += m_Regions[index].Size;
		const size_t instanceRegions = m_Regions.size();
		AddRuns(m_InstanceMarks, m_Instances.Buffer, reinterpret_cast<const uint8_t*>(instances.data()), sizeof(RenderData::InstanceData), c_InstanceRunGap,
			&update.InstanceRanges);
		for (size_t index = instanceRegions; index < m_Regions.size(); index++)
			update.InstanceBytes += m_Regions[index].Size;

		bool uploaded = true;
		if (!m_Regions.empty())
			uploaded = Renderer::GetUploadQueue().RecordSceneUpload(m_CommandList, m_Regions).Recorded;
		gpuTimer.End();
		m_CommandList->close();
		m_Device->executeCommandList(m_CommandList);
		m_Regions.clear();
		if (!uploaded)
		{
			// Staging could not be had. The versions stay, so the next update reads the same changes again; the proxies this
			// one made resident wait again (their records were not uploaded).
			ST_CORE_ERROR("RenderSceneGPU: no staging memory for the scene's data; the next update tries again");
			m_UploadAllTables = m_UploadAllTables || tablesChanged;
			for (const uint32_t proxy : m_Promoted)
			{
				m_Resident[proxy] = 0;
				m_Queued[proxy] = 1;
				m_Pending.push_back(proxy);
			}
			if (outUpdate)
				*outUpdate = RenderSceneGPUUpdate {};
			return false;
		}
		m_Initialized = true;
		m_ProxyVersion = renderScene.GetProxyVersion();
		m_InstanceVersion = renderScene.GetInstanceVersion();
		m_PaletteVersion = renderScene.GetPaletteVersion();
		m_LightVersion = lights.GetVersion();
		m_LightCapacity = lights.GetCapacity();
		if (outUpdate)
			*outUpdate = update;
		return true;
	}

	RenderSceneGPUCache::RenderSceneGPUCache(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	RenderSceneGPUCache::~RenderSceneGPUCache()
	{
		// The device is idle (Renderer::Shutdown): the mirrors go at once.
		std::lock_guard lock(s_Mutex);
		for (const auto& [id, mirror] : m_Mirrors)
			s_Mirrored.erase(id);
		s_Destroyed.clear();
	}

	RenderSceneGPU& RenderSceneGPUCache::Get(const RenderScene& renderScene)
	{
		const uint64_t id = renderScene.GetId();
		auto found = m_Mirrors.find(id);
		if (found == m_Mirrors.end())
		{
			{
				std::lock_guard lock(s_Mutex);
				s_Mirrored.insert(id);
			}
			// A clone's mirror is copied from its source's on the GPU when that holds exactly what the clone started with.
			Scope<RenderSceneGPU> mirror;
			RenderSceneGPU* source = renderScene.GetCloneSource() != 0 ? Find(renderScene.GetCloneSource()) : nullptr;
			if (source && source->HoldsVersions(renderScene.GetCloneVersions()))
				mirror = RenderSceneGPU::CreateClone(*source, id);
			if (!mirror)
				mirror = CreateScope<RenderSceneGPU>(m_Device, id);
			found = m_Mirrors.emplace(id, std::move(mirror)).first;
			found->second->BeginFrame(m_Frame);
		}
		return *found->second;
	}

	uint64_t RenderSceneGPUCache::GetBufferBytes() const
	{
		uint64_t bytes = 0;
		for (const auto& [id, mirror] : m_Mirrors)
			bytes += mirror->GetBufferBytes();
		return bytes;
	}

	RenderSceneGPU* RenderSceneGPUCache::Find(uint64_t renderSceneId) const
	{
		const auto found = m_Mirrors.find(renderSceneId);
		return found != m_Mirrors.end() ? found->second.get() : nullptr;
	}

	void RenderSceneGPUCache::NotifyDestroyed(uint64_t renderSceneId)
	{
		std::lock_guard lock(s_Mutex);
		// Only scenes that were mirrored: a process without a renderer collects nothing.
		if (s_Mirrored.erase(renderSceneId) > 0)
			s_Destroyed.push_back(renderSceneId);
	}

	void RenderSceneGPUCache::Release(uint64_t renderSceneId)
	{
		const auto found = m_Mirrors.find(renderSceneId);
		if (found == m_Mirrors.end())
			return;
		const RenderSceneGPU& mirror = *found->second;
		// Frames in flight may still read its buffers.
		std::vector<nvrhi::ResourceHandle> buffers;
		for (nvrhi::IBuffer* buffer : { mirror.GetInstanceBuffer(), mirror.GetProxyBuffer(), mirror.GetPaletteBuffer(), mirror.GetMeshTableBuffer(),
			mirror.GetMaterialBuffer(), mirror.GetMaterialFlagBuffer() })
		{
			if (buffer)
				buffers.emplace_back(buffer);
		}
		Renderer::ReleaseDeferred(std::move(buffers));
		m_Mirrors.erase(found);
	}

	void RenderSceneGPUCache::BeginFrame()
	{
		m_Frame++;
		std::vector<uint64_t> destroyed;
		{
			std::lock_guard lock(s_Mutex);
			destroyed.swap(s_Destroyed);
		}
		for (const uint64_t id : destroyed)
			Release(id);

		std::vector<uint64_t> unused;
		for (const auto& [id, mirror] : m_Mirrors)
		{
			if (m_Frame - mirror->GetLastUsedFrame() > c_UnusedFrames)
				unused.push_back(id);
			else
				mirror->BeginFrame(m_Frame);
		}
		for (const uint64_t id : unused)
		{
			{
				std::lock_guard lock(s_Mutex);
				s_Mirrored.erase(id);
			}
			Release(id);
		}
	}

}
