#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>

namespace Strata
{

	class AssetManagerBase;
	class AssetUploadContext;

	enum class AssetState : uint8_t
	{
		Unloaded = 0,
		Loading,  // Queued or being read/decoded on worker threads
		Ready,
		Failed
	};

	enum class AssetPriority : uint8_t
	{
		High = 0, // Needed right now (visible, blocking gameplay)
		Normal,
		Low       // Prefetch
	};

	const char* AssetStateToString(AssetState state);

	// Handles 1-255 are reserved for the engine's built-in assets (see BuiltinAssets).
	constexpr uint64_t c_MaxBuiltinAssetHandle = 0xFF;

	inline bool IsBuiltinAssetHandle(AssetHandle handle)
	{
		return handle.IsValid() && static_cast<uint64_t>(handle) <= c_MaxBuiltinAssetHandle;
	}

	// The most an asset that uploads in steps (AssetFinalizeContext) copies in one step: about a millisecond of memcpy. Steps
	// are sized to the time and bytes left in the frame, so most are smaller.
	constexpr uint64_t c_AssetUploadStepBytes = 4ull << 20;

	// What a call of Asset::FinalizeOnMainThread achieved.
	enum class AssetFinalizeResult : uint8_t
	{
		Done = 0, // Finalized: the asset can be published
		Pending,  // The call's upload budget ran out first: the asset continues in the next call (a later frame)
		Failed,
		// Every upload is recorded, but on a queue the GPU runs beside the frame's work (the renderer's transfer queue): the
		// asset continues in a later frame, once the GPU completed them, and the arrivals behind it go on meanwhile. Only when
		// the context allows it (AssetFinalizeContext::MayWait).
		AwaitingGPU
	};

	// What a call of Asset::FinalizeOnMainThread may do. Everything is unlimited unless the manager budgets the frame (loads
	// it finalizes in Update, see AssetResidencyBudgets); then a call may end Pending, and the asset continues in a later
	// frame.
	//
	// Assets that upload in steps of at most c_AssetUploadStepBytes (textures: runs of rows or several small levels; meshes:
	// ranges of their buffers) size each step to what is left of UploadBudget, of UploadMsBudget and of the time until
	// Deadline, at the speed of the latest steps (the renderer's GPUUploadQueue::PlanStep), and take none once not even a
	// granule (a row of blocks, 64 KiB) fits: a frame uploads no more than its byte budget, and its steps end close to the
	// time budget. Only the frame's first step (FirstOfFrame, nothing uploaded in the frame yet, *UploadedBytes == 0) goes
	// whatever the budgets say, one granule at least, so every frame makes progress and an asset larger than any budget
	// still arrives. Steps through staging memory also wait while the staging in flight leaves no room for them: the frame's
	// first step within StagingBytes, the later ones within GetStagingRoom (StagingBytes less one step, kept for the next
	// frame's first), so staging never exceeds StagingBytes (but for a single step larger than all of it, which goes once
	// nothing else is in flight). Staging becomes reusable once the GPU has run the copies that read it (the upload context
	// fences the uploads when it submits a batch, and the pool polls the fences): with the GPU keeping up every frame's
	// first step finds room, and a frame whose staging is all still in flight waits for the GPU (Pending without uploading,
	// as a later call of a frame may also end). The manager itself may also hold an arrival back until memory evicted for
	// it is released (see AssetManagerBase::Update).
	struct AssetFinalizeContext
	{
		// Where GPU uploads are recorded (Asset/AssetUploadContext.h); null without a graphics device: GPU assets then keep
		// their data on the CPU. The renderer's assets record only into the renderer's context, and finalize as without a
		// device when given another (a test's).
		AssetUploadContext* Upload = nullptr;
		AssetManagerBase* Manager = nullptr; // Manager publishing the asset (null for standalone assets)
		uint64_t UploadBudget = std::numeric_limits<uint64_t>::max(); // The call's share of the frame's upload bytes
		std::chrono::steady_clock::time_point Deadline = std::chrono::steady_clock::time_point::max();
		uint64_t StagingBytes = std::numeric_limits<uint64_t>::max(); // Staging memory uploads may keep in flight
		// When set, the GPU bytes the call uploaded are added to it (upload budgets and statistics); it holds what the
		// frame's earlier calls uploaded.
		uint64_t* UploadedBytes = nullptr;
		// The call's share of the frame's upload time (AssetResidencyBudgets::UploadMsPerFrame): the CPU time its steps may
		// take writing staging memory and recording copies. When UploadMs is set, the steps' time is added to it (it holds
		// the frame's earlier calls').
		float UploadMsBudget = std::numeric_limits<float>::infinity();
		float* UploadMs = nullptr;
		// The asset arrives as a dependency prefetch nobody has requested (see AssetManagerBase::GetDependencies): it must not
		// request what it references, because prefetches do not chain. The manager prefetches its dependencies once
		// something requests it, and whatever uses them requests them.
		bool Prefetched = false;
		// The manager calls again in a later frame (its budgeted updates): the asset may end AwaitingGPU. Without it (loads that
		// finish at once: LoadAssetSync, ReplaceLoadedAsset, AddMemoryAsset) an asset waits for its uploads itself.
		bool MayWait = false;
		// The frame's first finalization (and calls without a budget): its first step goes whatever the budgets and the
		// deadline say (one granule when they leave nothing), so every frame makes progress and an asset larger than a frame's
		// budget still arrives. A later call of the frame takes no step, and allocates nothing, once the deadline has passed,
		// and sizes its steps to end by it.
		bool FirstOfFrame = true;

		// The staging the steps after the frame's first may leave in flight: StagingBytes less one step, the room the next
		// frame's first step finds also when the GPU has not run a frame's uploads by then. A budget below one step allows
		// the frame's first step only.
		uint64_t GetStagingRoom() const { return StagingBytes > c_AssetUploadStepBytes ? StagingBytes - c_AssetUploadStepBytes : 0; }
	};

	// Memory an asset holds, per residency pool (see AssetResidencyBudgets): each pool has its own budget, because
	// running out of video memory and running out of system memory are different problems.
	struct AssetMemoryUsage
	{
		uint64_t Cpu = 0;         // System memory: decoded data kept on the CPU (geometry for physics, samples, documents)
		uint64_t GpuTextures = 0; // Video memory of textures
		uint64_t GpuBuffers = 0;  // Video memory of buffers (vertices, indices)

		uint64_t GetTotal() const { return Cpu + GpuTextures + GpuBuffers; }
		uint64_t GetGpu() const { return GpuTextures + GpuBuffers; }

		AssetMemoryUsage& operator+=(const AssetMemoryUsage& other)
		{
			Cpu += other.Cpu;
			GpuTextures += other.GpuTextures;
			GpuBuffers += other.GpuBuffers;
			return *this;
		}

		AssetMemoryUsage& operator-=(const AssetMemoryUsage& other)
		{
			Cpu -= other.Cpu;
			GpuTextures -= other.GpuTextures;
			GpuBuffers -= other.GpuBuffers;
			return *this;
		}

		bool operator==(const AssetMemoryUsage& other) const = default;
	};

	// Base class of every asset. Assets are shared (Ref<Asset>) and identified by their handle; code holds handles
	// and asks the asset manager for the current object, which allows hot reload and streaming to replace objects.
	// How much of an asset that can be partly resident is, in levels of detail (textures: mip levels, 0 the finest):
	// Asset::GetDetailLevels, AssetResidencyInfo::Detail.
	struct AssetDetailLevels
	{
		uint32_t Resident = 0;      // The finest level resident
		uint32_t Wanted = 0;        // The finest level something needs (Resident while nothing asks for another)
		uint32_t Count = 0;         // Levels the asset has
		uint64_t ResidentBytes = 0; // What the resident levels hold
	};

	class Asset
	{
	public:
		virtual ~Asset() = default;

		virtual AssetType GetType() const = 0;

		// Called on the main thread once the asset has been loaded, before it becomes Ready. GPU assets create their
		// GPU resources here and report what they uploaded (AssetFinalizeContext::UploadedBytes); assets referencing others
		// may request them from the context's manager. Must not block on other loads (no LoadAssetSync). Large uploads may
		// be split over several calls within the context's budget (Pending); a call with an unlimited budget finishes.
		virtual AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext&) { return AssetFinalizeResult::Done; }

		// Approximate memory held by the asset, per pool. Asset managers read it once the asset is finalized (for
		// residency budgets, upload budgets and statistics), so it must describe the asset's state from then on.
		virtual AssetMemoryUsage GetMemoryUsage() const { return {}; }
		// What GetMemoryUsage will report once the asset is finalized, asked before finalizing: managers make room for it in
		// their budgets first, so its memory is allocated after what it replaces was freed. Assets whose finalization moves
		// data between pools (CPU pixels to a GPU texture) override it.
		virtual AssetMemoryUsage GetFinalizedMemoryUsage() const { return GetMemoryUsage(); }

		// Whether objects outside the asset share its data, so that dropping the asset would free nothing (a playing
		// voice holds an audio clip's samples). Residency management keeps such assets resident.
		virtual bool IsDataShared() const { return false; }

		// For assets that can be partly resident (textures streaming their mip levels): how much of it is. False for the
		// others. Any thread.
		virtual bool GetDetailLevels(AssetDetailLevels&) const { return false; }

		AssetHandle Handle = UUID::Null();
	};

	struct AssetMetadata
	{
		AssetHandle Handle = UUID::Null();
		AssetType Type = AssetType::None;
		// Path relative to the project's asset directory with '/' separators ("Models/Helmet.gltf"). Sub-assets share
		// their parent's path; built-in assets use "Builtin/<Name>".
		std::string Path;
		AssetHandle Parent = UUID::Null(); // Owning asset for sub-assets (e.g. meshes inside a model)
		std::string SubAssetKey;           // Identifies a sub-asset within its parent ("Mesh/0")
		std::string Name;                  // Display name
		// Bytes of the stored form a load reads (the cooked data, or the source file for assets stored as-is); 0 when
		// unknown. The streaming queue bounds the bytes of loads in flight with it.
		uint64_t StoredSize = 0;

		bool IsSubAsset() const { return Parent.IsValid(); }
		bool IsBuiltin() const { return IsBuiltinAssetHandle(Handle); }
	};

}
