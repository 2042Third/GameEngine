#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Renderer/MaterialData.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strata
{

	class AssetManagerBase;

	// What one RenderSceneGPU::Update uploaded.
	struct RenderSceneGPUUpdate
	{
		uint64_t InstanceBytes = 0;  // Instance data (RenderData::InstanceData)
		uint32_t InstanceRanges = 0; // Separate ranges of it
		uint64_t ProxyBytes = 0;     // Culling records (RenderData::GPUProxy)
		uint64_t PaletteBytes = 0;   // Joint matrices of posed skinned proxies
		uint32_t NewProxies = 0;     // Proxies that became drawable (resident)
		uint32_t MeshesChecked = 0;  // Mesh slots whose table entry was worked out again (content changes, new slots)
		uint32_t MaterialsBuilt = 0; // Material slots whose data was built again
		uint64_t MaterialBytes = 0;  // Material data uploaded (RenderData::MaterialData)
		uint64_t TableBytes = 0;     // Mesh table entries and material flags uploaded
		uint64_t LightBytes = 0;     // Light records (RenderData::LightRecord)
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
	// The buffers grow by a GPU copy into larger ones, so growth uploads nothing again.
	//
	// The GPU's culling draws what is resident, whatever a frame requested, so the mirror also keeps the scene's mesh table
	// (per mesh slot the GeometryHandle of the loaded mesh, with c_CullMeshSkinnedArena for the Skinned arena; 0 while it is
	// not loaded) and its material data and flags (per material slot, of the loaded material, or of the default material
	// while it is not loaded; flags 0 while neither is), naming textures by texture table index (a map not uploaded yet has
	// its fallback). They follow the asset manager's content changes and the scene's slots (GetSlotsVersion): a material is
	// built again when it, the default material or one of its textures was loaded, reloaded or dropped (a reverse index from
	// textures to their materials), never because a texture streamed levels. Nothing here requests or stamps an asset:
	// the renderers do. Main thread only.
	class RenderSceneGPU
	{
	public:
		RenderSceneGPU(nvrhi::IDevice* device, uint64_t renderSceneId);
		~RenderSceneGPU();

		RenderSceneGPU& operator=(const RenderSceneGPU&) = delete;

		// The mirror of a clone of the source's render scene (RenderScene::CopyIntoScene): the source's state with its buffers
		// copied on the GPU (recorded and executed at once), so the clone's first frame uploads nothing. Null when a buffer
		// could not be made (logged): the clone's mirror is then filled by uploads.
		static Scope<RenderSceneGPU> CreateClone(RenderSceneGPU& source, uint64_t renderSceneId);
		// Whether the mirror holds what its render scene's change logs stood at at `versions`, and nothing later: a clone made
		// then can take it over.
		bool HoldsVersions(const RenderScene::ChangeVersions& versions) const;
		// The bytes of its buffers (the GPU memory the mirror holds).
		uint64_t GetBufferBytes() const;

		uint64_t GetRenderSceneId() const { return m_RenderSceneId; }

		// Brings the buffers up to date with the render scene (synced first), recording the copies into a command list of its
		// own that it executes at once, so that work recorded later reads them. New proxies upload while the frame's budget of
		// `newProxyBytesPerFrame` (records and instances) lasts, at least one per frame. Returns false when a buffer could not
		// be made (logged): the buffers keep what they held, and a later update tries again.
		bool Update(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets, uint64_t newProxyBytesPerFrame, RenderSceneGPUUpdate* outUpdate = nullptr);
		// A device frame began (RenderSceneGPUCache::BeginFrame): the budget for new proxies starts again.
		void BeginFrame(uint64_t frame);

		// Whether the proxy's record and instances are on the GPU: renderers draw resident proxies only.
		bool IsResident(uint32_t proxy) const { return proxy < m_Resident.size() && m_Resident[proxy] != 0; }
		// The records and instances of proxies waiting for their upload.
		uint64_t GetDeferredBytes() const { return m_DeferredBytes; }
		uint32_t GetDeferredProxies() const { return m_DeferredProxies; }

		// Whether every buffer exists (a failed update keeps those an earlier one made): renderers draw only then.
		bool HasBuffers() const
		{
			return m_Instances.Buffer && m_Proxies.Buffer && m_Palette.Buffer && m_MeshTableBuffer.Buffer && m_MaterialBuffer.Buffer && m_MaterialFlagBuffer.Buffer;
		}
		nvrhi::IBuffer* GetInstanceBuffer() const { return m_Instances.Buffer; }
		nvrhi::IBuffer* GetProxyBuffer() const { return m_Proxies.Buffer; }
		nvrhi::IBuffer* GetPaletteBuffer() const { return m_Palette.Buffer; }
		nvrhi::IBuffer* GetMeshTableBuffer() const { return m_MeshTableBuffer.Buffer; }
		nvrhi::IBuffer* GetMaterialBuffer() const { return m_MaterialBuffer.Buffer; }
		nvrhi::IBuffer* GetMaterialFlagBuffer() const { return m_MaterialFlagBuffer.Buffer; }
		// The render scene's light records (RenderLights) as of the latest update: GetLightCapacity of them (null without any).
		nvrhi::IBuffer* GetLightRecordBuffer() const { return m_LightRecords.Buffer; }
		uint32_t GetLightCapacity() const { return m_LightCapacity; }

		// Changes whenever an update built a mesh or material slot again: what was culled with an earlier version of the tables
		// (visibility feedback) may be out of date.
		uint64_t GetTablesVersion() const { return m_TablesVersion; }
		// The tables as the buffers hold them after the latest update.
		uint32_t GetMeshTableEntry(uint32_t slot) const { return slot < m_MeshTable.size() ? m_MeshTable[slot] : 0u; }
		uint32_t GetMaterialFlags(uint32_t slot) const { return slot < m_MaterialFlags.size() ? m_MaterialFlags[slot] : 0u; }
		const RenderData::MaterialData* GetMaterialData(uint32_t slot) const { return slot < m_MaterialData.size() ? &m_MaterialData[slot] : nullptr; }
		uint32_t GetMeshRecords(uint32_t slot) const { return slot < m_MeshRecords.size() ? m_MeshRecords[slot] : 0u; }
		uint32_t GetMeshSlotCount() const { return static_cast<uint32_t>(m_MeshTable.size()); }
		uint32_t GetMaterialSlotCount() const { return static_cast<uint32_t>(m_MaterialFlags.size()); }
		// The LOD records (draw records) of the loaded meshes of an arena (0: Static, 1: Skinned), and whether any material the
		// mirror draws is double-sided or alpha-masked: the buckets a frame can have.
		uint32_t GetArenaRecords(uint32_t arena) const { return m_ArenaRecords[arena]; }
		bool HasDoubleSidedMaterials() const { return m_DoubleSidedMaterials > 0; }
		bool HasAlphaMaskedMaterials() const { return m_AlphaMaskedMaterials > 0; }
		// The material slots whose flags say transparent, in no order: the GPU's culling leaves their instances to the CPU.
		std::span<const uint32_t> GetTransparentMaterialSlots() const { return m_TransparentSlots; }
		// Changes whenever a buffer is replaced, to a value no mirror had before: binding sets made for another value are
		// made again.
		uint64_t GetBufferVersion() const { return m_BufferVersion; }
		// The frame (BeginFrame) of the latest update.
		uint64_t GetLastUsedFrame() const { return m_LastUsedFrame; }

		// Elements the instance buffer and the palette buffer have room for (RenderData::InstanceData, joint matrices).
		uint64_t GetInstanceCapacity() const { return m_Instances.ElementCount; }
		uint64_t GetPaletteCapacity() const { return m_Palette.ElementCount; }
		// What the updates wrote into the instance and palette buffers (changes, new proxies' instances, whole uploads), by write
		// version: a reader that keeps a copy of a buffer as of a version (a renderer's motion history) copies these ranges
		// again. Each log remembers at most as many written elements as its buffer has room for, and c_MaxWriteLogEntries
		// ranges: a reader further behind copies the whole buffer, which costs no more. Write versions are unique in the
		// process and a mirror's logs start at its creation, so a version taken before a mirror was made (a scene's mirror made
		// again, a clone filled by uploads) never reads as up to date; a clone made by GPU copies keeps its source's logs, as its
		// buffers hold what the source's did, but only up to the copy: the source's later writes are not in the clone's buffers.
		uint64_t GetWriteVersion() const { return m_WriteVersion; }
		// Appends the ranges (first element, count) written after `sinceVersion`, newest first, possibly overlapping:
		// `sinceVersion` is a write version the reader took from the mirror of render scene `renderSceneId`, this mirror's
		// scene or, for a clone made by GPU copies, its source's. Returns false, appending nothing, when the log does not reach
		// back that far or the version is not one this mirror's buffers went through (the source's after the copy): then any
		// element may differ.
		bool GetInstanceWrites(uint64_t renderSceneId, uint64_t sinceVersion, std::vector<std::pair<uint32_t, uint32_t>>& outRanges) const;
		bool GetPaletteWrites(uint64_t renderSceneId, uint64_t sinceVersion, std::vector<std::pair<uint32_t, uint32_t>>& outRanges) const;
		static constexpr size_t c_MaxWriteLogEntries = 65536;

		// The record of a proxy as the GPU's culling reads it.
		static RenderData::GPUProxy MakeRecord(const RenderScene& renderScene, uint32_t proxy);
	private:
		struct GrowingBuffer
		{
			nvrhi::BufferHandle Buffer;
			uint64_t ElementCount = 0; // What the buffer has room for
		};

		// The ranges an update wrote into a buffer (GetInstanceWrites, GetPaletteWrites).
		struct WriteLog
		{
			struct Entry
			{
				uint64_t Version = 0;
				uint32_t First = 0;
				uint32_t Count = 0;
			};
			std::vector<Entry> Entries;
			size_t Start = 0;      // Entries before this are dropped (compacted when they make up half the list)
			uint64_t Elements = 0; // The elements of the live entries
			uint64_t Floor = 0;    // Writes up to this version may be missing

			// Logs a write, dropping the oldest entries beyond `capacity` elements or c_MaxWriteLogEntries entries.
			void Record(uint64_t version, uint32_t first, uint32_t count, uint64_t capacity);
			bool Get(uint64_t sinceVersion, uint64_t currentVersion, std::vector<std::pair<uint32_t, uint32_t>>& outRanges) const;
		};
		// Logs the instance and palette regions of an update that was recorded (m_Regions), under a new write version.
		void RecordWrites();
		// Whether a write version taken from the mirror of render scene `renderSceneId` is one this mirror's buffers held: its
		// own, or its source's up to the copy that made it.
		bool IsOwnWriteVersion(uint64_t renderSceneId, uint64_t version) const;

		// Grows the buffer to `elementCount` elements by a GPU copy (new space cleared to zeros where `clear`). False when it
		// cannot be created (logged).
		bool EnsureCapacity(GrowingBuffer& buffer, uint64_t elementCount, uint32_t elementSize, const char* name, bool clear);
		// Adds runs of the marked elements (sorted, each once) as upload regions: elements close together go in one run.
		void AddRuns(std::vector<uint32_t>& marks, nvrhi::IBuffer* buffer, const uint8_t* data, uint32_t elementSize, uint32_t gap, uint32_t* outRuns);
		void MarkRecord(uint32_t proxy);
		void MarkInstances(uint32_t first, uint32_t count);
		// Whether the asset tables may have to change: another manager, content changes or slot changes since the last update.
		bool AssetTablesChanged(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets) const;
		// The mesh table and the material data and flags after the asset manager's content changes and the scene's slot
		// changes, marking the entries to upload (m_MeshMarks, m_MaterialMarks).
		void UpdateAssetTables(const RenderScene& renderScene, const Ref<AssetManagerBase>& assets, RenderSceneGPUUpdate& update);
		void BuildMeshEntry(uint32_t slot, AssetHandle mesh, const AssetManagerBase* assets);
		void BuildMaterial(uint32_t slot, AssetHandle material, const AssetManagerBase* assets);
		void SetMaterialFlags(uint32_t slot, uint32_t flags);
		void RemoveTextureUsers(uint32_t slot);
		// Every member as the source has it (CreateClone then gives the clone buffers and a command list of its own).
		RenderSceneGPU(const RenderSceneGPU& source) = default;
	private:
		nvrhi::IDevice* m_Device;
		uint64_t m_RenderSceneId;
		nvrhi::CommandListHandle m_CommandList;
		GrowingBuffer m_Instances;
		GrowingBuffer m_Proxies;
		GrowingBuffer m_Palette;
		uint64_t m_BufferVersion = 0;
		uint64_t m_WriteVersion = 0;
		// A clone made by GPU copies: its source's render scene and write version at the copy (0 for other mirrors).
		uint64_t m_CloneSourceId = 0;
		uint64_t m_CloneWriteVersion = 0;
		WriteLog m_InstanceWrites;
		WriteLog m_PaletteWrites;
		bool m_Initialized = false; // The first update reads everything

		// The versions of the render scene the buffers hold.
		uint64_t m_ProxyVersion = 0;
		uint64_t m_InstanceVersion = 0;
		uint64_t m_PaletteVersion = 0;
		uint64_t m_LightVersion = 0;
		uint32_t m_LightCapacity = 0; // Records the light buffer holds as of the latest update
		GrowingBuffer m_LightRecords;
		std::vector<uint32_t> m_LightMarks;

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

		// The asset tables (see the class comment): what each slot was built for, and what the GPU holds.
		std::weak_ptr<AssetManagerBase> m_Assets;
		bool m_HasAssets = false;               // m_Assets names the manager the tables were built from
		uint64_t m_AssetContentVersion = 0;
		uint64_t m_SlotsVersion = ~0ull;
		uint64_t m_TablesVersion = 0;
		bool m_UploadAllTables = false;         // An upload of the tables failed: the next one writes them whole
		std::vector<AssetHandle> m_MeshSlotHandles;
		std::vector<uint32_t> m_MeshTable;
		std::vector<uint32_t> m_MeshRecords;           // Per mesh slot: the LOD records of its loaded mesh
		std::array<uint32_t, 2> m_ArenaRecords = {};
		std::vector<AssetHandle> m_MaterialSlotHandles;
		std::vector<RenderData::MaterialData> m_MaterialData;
		std::vector<uint32_t> m_MaterialFlags;
		std::vector<uint8_t> m_MaterialUsesDefault;    // Drawn with the default material while its own is not loaded
		// Per material slot the distinct textures its data was built with, and each one's place in its m_TextureUsers list
		// (so that a slot leaves the lists in constant time).
		std::vector<std::array<AssetHandle, MaterialGPU::c_TextureMapCount>> m_MaterialTextures;
		std::vector<std::array<uint32_t, MaterialGPU::c_TextureMapCount>> m_MaterialTexturePositions;
		std::unordered_map<AssetHandle, std::vector<uint32_t>> m_TextureUsers; // Texture -> material slots naming it
		uint32_t m_DoubleSidedMaterials = 0;
		uint32_t m_AlphaMaskedMaterials = 0;
		std::vector<uint32_t> m_TransparentSlots;
		std::vector<uint32_t> m_TransparentPositions; // Per material slot: its place in m_TransparentSlots (~0: not listed)
		GrowingBuffer m_MeshTableBuffer;
		GrowingBuffer m_MaterialBuffer;
		GrowingBuffer m_MaterialFlagBuffer;
		std::vector<uint32_t> m_MeshMarks;
		std::vector<uint32_t> m_MaterialMarks;
		std::vector<AssetHandle> m_ContentChanges;

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
		// The GPU memory every mirror's buffers hold (a clone's mirror doubles a scene's while it plays).
		uint64_t GetBufferBytes() const;

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
