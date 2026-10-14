#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strata
{

	class RenderScene;

	// What one RenderSceneGPU::Update uploaded.
	struct RenderSceneGPUUpdate
	{
		uint64_t InstanceBytes = 0;  // Instance data (RenderData::InstanceData)
		uint32_t InstanceRanges = 0; // Separate ranges of it
		uint64_t ProxyBytes = 0;     // Culling records (RenderData::GPUProxy)
		uint64_t PaletteBytes = 0;   // Joint matrices of posed skinned proxies
		uint32_t NewProxies = 0;     // Proxies that became drawable (resident)
	};

	// The GPU's copy of a render scene (RenderScene), shared by every renderer of the scene: its instance data, a culling
	// record per proxy (RenderData::GPUProxy) and the palettes of posed skinned proxies, in buffers that persist between frames.
	// Update follows the scene's change logs (GetProxyChanges, GetInstanceChanges, GetPaletteChanges) and uploads what changed
	// through staging (GPUUploadQueue::RecordSceneUpload), never all of it again: a frame that changes nothing uploads
	// nothing, and a second renderer of the scene finds everything uploaded.
	//
	// A proxy is drawn only once it is resident: its record and instances on the GPU, uploaded in the same update. Changes of
	// resident proxies upload at once, every update (a moved object never lags); new proxies (a scene's first frame, entities
	// created, a proxy index taken over) upload within the frame's budget for them (BeginFrame), oldest first, and wait
	// meanwhile (GetDeferredBytes) with a record of zeros, which draws nothing. A destroyed proxy's record is cleared at once.
	// The buffers grow by a GPU copy into larger ones, so growth uploads nothing again. Main thread only.
	class RenderSceneGPU
	{
	public:
		RenderSceneGPU(nvrhi::IDevice* device, uint64_t renderSceneId);
		~RenderSceneGPU();

		RenderSceneGPU(const RenderSceneGPU&) = delete;
		RenderSceneGPU& operator=(const RenderSceneGPU&) = delete;

		uint64_t GetRenderSceneId() const { return m_RenderSceneId; }

		// Brings the buffers up to date with the render scene (synced first), recording the copies into a command list of its
		// own that it executes at once, so that work recorded later reads them. New proxies upload while the frame's budget of
		// `newProxyBytesPerFrame` (records and instances) lasts, at least one per frame. Returns false when a buffer could not
		// be made (logged): the buffers keep what they held, and a later update tries again.
		bool Update(const RenderScene& renderScene, uint64_t newProxyBytesPerFrame, RenderSceneGPUUpdate* outUpdate = nullptr);
		// A device frame began (RenderSceneGPUCache::BeginFrame): the budget for new proxies starts again.
		void BeginFrame(uint64_t frame);

		// Whether the proxy's record and instances are on the GPU: renderers draw resident proxies only.
		bool IsResident(uint32_t proxy) const { return proxy < m_Resident.size() && m_Resident[proxy] != 0; }
		// The records and instances of proxies waiting for their upload.
		uint64_t GetDeferredBytes() const { return m_DeferredBytes; }
		uint32_t GetDeferredProxies() const { return m_DeferredProxies; }

		nvrhi::IBuffer* GetInstanceBuffer() const { return m_Instances.Buffer; }
		nvrhi::IBuffer* GetProxyBuffer() const { return m_Proxies.Buffer; }
		nvrhi::IBuffer* GetPaletteBuffer() const { return m_Palette.Buffer; }
		// Changes whenever a buffer is replaced, to a value no mirror had before: binding sets made for another value are
		// made again.
		uint64_t GetBufferVersion() const { return m_BufferVersion; }
		// The frame (BeginFrame) of the latest update.
		uint64_t GetLastUsedFrame() const { return m_LastUsedFrame; }

		// The record of a proxy as the GPU's culling reads it.
		static RenderData::GPUProxy MakeRecord(const RenderScene& renderScene, uint32_t proxy);
	private:
		struct GrowingBuffer
		{
			nvrhi::BufferHandle Buffer;
			uint64_t ElementCount = 0; // What the buffer has room for
		};

		// Grows the buffer to `elementCount` elements by a GPU copy (new space cleared to zeros where `clear`). False when it
		// cannot be created (logged).
		bool EnsureCapacity(GrowingBuffer& buffer, uint64_t elementCount, uint32_t elementSize, const char* name, bool clear);
		// Adds runs of the marked elements (sorted, each once) as upload regions: elements close together go in one run.
		void AddRuns(std::vector<uint32_t>& marks, nvrhi::IBuffer* buffer, const uint8_t* data, uint32_t elementSize, uint32_t gap, uint32_t* outRuns);
		void MarkRecord(uint32_t proxy);
		void MarkInstances(uint32_t first, uint32_t count);
	private:
		nvrhi::IDevice* m_Device;
		uint64_t m_RenderSceneId;
		nvrhi::CommandListHandle m_CommandList;
		GrowingBuffer m_Instances;
		GrowingBuffer m_Proxies;
		GrowingBuffer m_Palette;
		uint64_t m_BufferVersion = 0;
		bool m_Initialized = false; // The first update reads everything

		// The versions of the render scene the buffers hold.
		uint64_t m_ProxyVersion = 0;
		uint64_t m_InstanceVersion = 0;
		uint64_t m_PaletteVersion = 0;

		// Per proxy: its record as uploaded (or to be uploaded this update), whether it is resident, whether it waits in
		// m_Pending, and the update that last looked at it (each proxy once per update).
		std::vector<RenderData::GPUProxy> m_Records;
		std::vector<uint8_t> m_Resident;
		std::vector<uint8_t> m_Queued;
		std::vector<uint32_t> m_Seen;
		uint32_t m_UpdateStamp = 0;
		// Per instance: the proxy whose range last held it (instance changes of proxies that are not resident wait for them).
		std::vector<uint32_t> m_InstanceOwners;
		// New proxies waiting for their upload, oldest first (from m_PendingHead on).
		std::vector<uint32_t> m_Pending;
		size_t m_PendingHead = 0;
		uint64_t m_DeferredBytes = 0;
		uint32_t m_DeferredProxies = 0;

		uint64_t m_Frame = 0;
		uint64_t m_LastUsedFrame = 0;
		uint64_t m_BudgetFrame = ~0ull; // The frame m_BudgetLeft belongs to
		uint64_t m_BudgetLeft = 0;
		uint32_t m_FrameNewProxies = 0;

		// Scratch of an update, reused.
		std::vector<uint32_t> m_Changes;
		std::vector<uint32_t> m_RecordMarks;
		std::vector<uint32_t> m_InstanceMarks;
		std::vector<uint32_t> m_Promoted; // Proxies the update made resident
		std::vector<std::pair<uint32_t, uint32_t>> m_PaletteRanges;
		std::vector<GPUBufferRegion> m_Regions;
	};

	// The renderer's GPU mirrors of render scenes (Renderer::GetRenderSceneGPUs), by RenderScene::GetId: a scene's renderers
	// share its mirror. Not in the scene's registry context: scenes are destroyed on background jobs
	// (Scene::ReleaseInBackground), and GPU resources are released on the main thread. A destroyed render scene posts its
	// id (NotifyDestroyed, any thread), and BeginFrame releases its mirror, as well as mirrors no renderer updated for
	// c_UnusedFrames frames. Main thread only, but NotifyDestroyed.
	class RenderSceneGPUCache
	{
	public:
		static constexpr uint64_t c_UnusedFrames = 300;

		explicit RenderSceneGPUCache(nvrhi::IDevice* device);
		~RenderSceneGPUCache();

		RenderSceneGPUCache(const RenderSceneGPUCache&) = delete;
		RenderSceneGPUCache& operator=(const RenderSceneGPUCache&) = delete;

		// The render scene's mirror, created on first use.
		RenderSceneGPU& Get(const RenderScene& renderScene);
		RenderSceneGPU* Find(uint64_t renderSceneId) const;
		size_t GetMirrorCount() const { return m_Mirrors.size(); }

		// Once per device frame (Renderer::BeginFrame): releases the mirrors of destroyed render scenes and those unused for
		// c_UnusedFrames frames (their buffers once the GPU is done with them), and starts the mirrors' budgets for new proxies.
		void BeginFrame();

		// A render scene was destroyed (its destructor, on any thread): its mirror goes at the next BeginFrame.
		static void NotifyDestroyed(uint64_t renderSceneId);
	private:
		void Release(uint64_t renderSceneId);
	private:
		nvrhi::IDevice* m_Device;
		std::unordered_map<uint64_t, Scope<RenderSceneGPU>> m_Mirrors;
		uint64_t m_Frame = 0;

		// The ids that have mirrors, and those destroyed since the last BeginFrame (render scenes die on any thread).
		static std::mutex s_Mutex;
		static std::unordered_set<uint64_t> s_Mirrored;
		static std::vector<uint64_t> s_Destroyed;
	};

}
