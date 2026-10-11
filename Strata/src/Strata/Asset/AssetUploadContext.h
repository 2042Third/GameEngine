#pragma once

#include "Strata/Core/Base.h"

#include <cstdint>

namespace Strata
{

	// What GPU assets record their uploads into while they are finalized (AssetFinalizeContext::Upload), without the asset
	// layer knowing the graphics API. The renderer implements it (RendererUploadContext, which its assets downcast to, and
	// which records into its budgeted upload queue) and hands it to the asset managers through the IAssetGraphics engine
	// service; tests implement it to drive the finalize contract without a GPU. A manager opens a batch before the
	// finalizations of an update (and of LoadAssetSync, ReplaceLoadedAsset and AddMemoryAsset) and submits it after them, so
	// what a batch recorded reaches the GPU before anything renders with the assets it finalized. Main thread only.
	class AssetUploadContext
	{
	public:
		virtual ~AssetUploadContext() = default;

		// Opens a batch: the finalizations until Submit record into it. A batch opened while one is open (a memory asset
		// added by code a finalization runs) joins it: the outermost Submit submits.
		virtual void Begin() = 0;
		// Submits what the batch recorded (nothing when it recorded nothing).
		virtual void Submit() = 0;
		// Once per manager update, after its finalizations: lets the context return memory it no longer needs (idle staging).
		// Returns whether it returned staging memory that had become idle (AssetManagerStats::StagingReleases).
		virtual bool EndFrame(uint64_t uploadedBytes) = 0;
	};

	// What the asset layer needs of the graphics device, as an engine service (Core/EngineServices.h) the renderer module
	// registers: the asset managers ask it for the upload context of the running renderer, for the frames the device may be
	// working on (their eviction grace) and for its memory budget (their default budgets). Without the service, or while no
	// renderer runs, assets are finalized without a device: GPU assets keep their data on the CPU.
	class IAssetGraphics
	{
	public:
		virtual ~IAssetGraphics() = default;

		// The upload context of the running renderer; null while none runs.
		virtual AssetUploadContext* GetUploadContext() = 0;
		// The frames the device may be working on at once; 0 while no renderer runs.
		virtual uint32_t GetFramesInFlight() const = 0;
		// The device's memory budget in bytes (the device-local memory the process may use without hurting performance); 0
		// while no renderer runs or when the device does not report it.
		virtual uint64_t GetMemoryBudget() const = 0;
	};

}
