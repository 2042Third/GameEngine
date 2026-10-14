#include "stpch.h"
#include "Strata/Renderer/RenderSceneGPU.h"

#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <cstring>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_NoOwner = ~0u;
		// Changed instances (or records) this close together are uploaded in one run: fewer copies for a little more data.
		constexpr uint32_t c_InstanceRunGap = 16;
		constexpr uint32_t c_RecordRunGap = 16;

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
		if (clear)
			m_CommandList->clearBufferUInt(created, 0);
		if (buffer.Buffer)
		{
			// What it holds moves over on the GPU; frames in flight may still read the old buffer.
			m_CommandList->copyBuffer(created, 0, buffer.Buffer, 0, buffer.ElementCount * elementSize);
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

	bool RenderSceneGPU::Update(const RenderScene& renderScene, uint64_t newProxyBytesPerFrame, RenderSceneGPUUpdate* outUpdate)
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
		const bool changed = !m_Initialized || renderScene.GetProxyVersion() != m_ProxyVersion || renderScene.GetInstanceVersion() != m_InstanceVersion
			|| renderScene.GetPaletteVersion() != m_PaletteVersion;
		const bool pendingRoom = m_PendingHead < m_Pending.size() && (m_BudgetLeft > 0 || m_FrameNewProxies == 0);
		if (!changed && !pendingRoom)
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
			&& EnsureCapacity(m_Palette, palette.size(), sizeof(RenderData::AffineTransform), "Palette", false);
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
			found = m_Mirrors.emplace(id, CreateScope<RenderSceneGPU>(m_Device, id)).first;
			found->second->BeginFrame(m_Frame);
		}
		return *found->second;
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
		for (nvrhi::IBuffer* buffer : { mirror.GetInstanceBuffer(), mirror.GetProxyBuffer(), mirror.GetPaletteBuffer() })
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
