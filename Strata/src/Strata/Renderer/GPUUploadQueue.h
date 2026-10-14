#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Core/Base.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/TextureFormat.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace Strata
{

	class StagingPool;

	// A run of rows of one mip level, copied from CPU memory into a texture (GPUUploadQueue::UploadTexture).
	struct GPUTextureRegion
	{
		uint32_t MipLevel = 0;  // Of the destination texture
		uint32_t Width = 0;     // The level's size in texels
		uint32_t Height = 0;
		uint32_t FirstRow = 0;  // Texel rows; for block-compressed formats the first row of a block. 0: the level's first copy
		uint32_t Rows = 0;      // Texel rows; for block-compressed formats whole blocks but at the level's end
		const uint8_t* Data = nullptr; // The first row's texels (or blocks), rows tightly packed
	};

	// A byte range copied from CPU memory into a buffer (GPUUploadQueue::UploadBuffer).
	struct GPUBufferRegion
	{
		nvrhi::IBuffer* Buffer = nullptr;
		uint64_t Offset = 0;
		const uint8_t* Data = nullptr;
		uint64_t Size = 0;
	};

	// The queue an upload's copies run on (GPUUploadQueue::ChooseQueue). An upload keeps the queue it started on to its end.
	enum class GPUUploadQueueKind : uint8_t
	{
		Graphics = 0, // Ahead of the frame's rendering: what it uploaded may be used as soon as its batch was submitted
		Transfer      // Beside the frame's work: used once complete and handed over (ReleaseToGraphics, AcquireOnGraphics)
	};

	// What an upload step recorded (GPUUploadQueue::UploadTexture, UploadBuffer).
	struct GPUUploadStep
	{
		bool Recorded = false;     // False when no staging could be had: nothing was recorded
		uint64_t Bytes = 0;        // Bytes it copies
		float Milliseconds = 0.0f; // CPU time it took
	};

	// Identifies recorded uploads: complete once the GPU has run every copy recorded before it was taken
	// (GPUUploadQueue::IsComplete).
	using GPUUploadTicket = uint64_t;

	// What the queue did in a frame (between two BeginFrame calls) and since it was created.
	struct GPUUploadQueueStats
	{
		uint64_t FrameBytes = 0;     // Bytes copied from staging in the frame so far
		float FrameMs = 0.0f;        // CPU time of the frame's steps: writing staging memory and recording the copies
		uint32_t FrameSteps = 0;
		uint64_t LastFrameBytes = 0; // The previous frame's
		float LastFrameMs = 0.0f;
		uint64_t TextureSteps = 0;   // Since the queue was created
		uint64_t BufferSteps = 0;
		uint64_t TextureBytes = 0;
		uint64_t BufferBytes = 0;
		uint64_t TransferSteps = 0;  // Of the steps, those on the transfer queue
		uint64_t TextureCopies = 0;  // GPU-side copies of texture levels (CopyTextureLevels)
		uint64_t Batches = 0;        // Batches submitted
		// What sizes the steps (PlanStep): the measured speed of steps through staging the pool had, of those through staging
		// created for them (its pages fault in as they are first written, which the system serializes), and the time creating
		// and mapping a staging buffer takes (a spike counts at once and decays over the next creations: the driver's
		// allocations take from tenths of a millisecond to a millisecond, rarely).
		float BytesPerMs = 0.0f;
		float ColdBytesPerMs = 0.0f;
		float StagingCreateMs = 0.0f;
		bool TransferQueue = false;  // New uploads go to the transfer queue
	};

	// Every asset upload goes through this queue (Renderer::GetUploadQueue): textures in runs of rows of their levels or
	// several small levels at once, meshes in ranges of their buffers, all through staging memory of the renderer's
	// StagingPool. The asset managers' batches (RendererUploadContext) open and submit its command lists; GPU assets plan their
	// steps with PlanStep, which sizes them by the measured speed of earlier steps to end within what is left of the frame's
	// upload time and bytes (AssetFinalizeContext::UploadMsBudget, UploadBudget) and of its finalization deadline, so a
	// frame's first step is no larger exception than one granule (a row of blocks, a small level). Uploads report their bytes
	// and time into the context.
	//
	// The copies run on the graphics queue ahead of the frame's rendering, so an asset may be used as soon as the batch with
	// its last step was submitted; or, with the transfer queue on (SetTransferQueue, where the device has one), uploads the
	// managers may wait for (AssetFinalizeContext::MayWait) run on the device's dedicated transfer queue, beside the frame's
	// work: their resources are handed over to the graphics queue by a queue family ownership transfer (released after the
	// last copy, ReleaseToGraphics, and acquired by the graphics queue once the release completed, AcquireOnGraphics), and the
	// assets wait for that meanwhile (AssetFinalizeResult::AwaitingGPU). Completion is tracked per batch with event queries
	// on each queue it used (as DeferredReleaseQueue does): GetTicket, IsComplete. Main thread only.
	class GPUUploadQueue
	{
	public:
		// The largest step: about a millisecond of writing staging memory.
		static constexpr uint64_t c_MaxStepBytes = c_AssetUploadStepBytes;
		// The smallest step where an upload has that much left (a frame's first step gets this much at least): steps much
		// smaller would spend their time on the step rather than on the copy. Buffer steps come in powers of two from this
		// size on, like the staging buffers they go through.
		static constexpr uint64_t c_MinStepBytes = 64ull << 10;

		GPUUploadQueue(GraphicsDevice& device, StagingPool& staging);
		~GPUUploadQueue();

		GPUUploadQueue(const GPUUploadQueue&) = delete;
		GPUUploadQueue& operator=(const GPUUploadQueue&) = delete;

		// New uploads the managers may wait for go to the transfer queue (true) or the graphics queue. Uploads under way keep
		// their queue. Returns whether the transfer queue is in use now (false without one on the device). Outside batches.
		bool SetTransferQueue(bool enabled);
		bool UsesTransferQueue() const { return m_UseTransferQueue; }
		// The queue a new upload under this context goes to: the transfer queue when it is in use and the manager will call
		// again (AssetFinalizeContext::MayWait), else the graphics queue.
		GPUUploadQueueKind ChooseQueue(const AssetFinalizeContext& context) const;

		// Opens a batch: uploads recorded until SubmitBatch belong to it. Its command lists open with the first thing recorded.
		void BeginBatch();
		// Executes what the batch recorded (the transfer queue's copies, then the graphics queue's work) and fences the staging
		// each released, so that the staging becomes reusable as soon as the GPU has run the copies rather than once it
		// finished the frame.
		void SubmitBatch();
		bool IsBatchOpen() const { return m_BatchOpen; }

		// Once per device frame (Renderer::BeginFrame): the frame's statistics, and completion of earlier batches.
		void BeginFrame();

		// The bytes the next step of an upload may take: at most `wantedBytes` and c_MaxStepBytes, within what is left of the
		// call's upload bytes, of its upload time less a tenth of the frame's (AssetFinalizeContext::UploadMsFrameBudget: a
		// reserve for steps that run slower than planned) and of nine tenths of the time until the deadline, at the speed of
		// the latest steps (`callBytes` and `callMs`: what the call uploaded and spent so far; a step the pool has no idle
		// staging for is planned with the time creating it takes, at most half the time left, and the speed of writing fresh
		// staging), rounded down to whole `granuleBytes`. 0 when not even one granule fits, except for the frame's first step
		// (the frame's first call, nothing uploaded in the frame yet), which gets one granule whatever: every frame makes
		// progress.
		uint64_t PlanStep(const AssetFinalizeContext& context, uint64_t callBytes, float callMs, uint64_t wantedBytes, uint64_t granuleBytes) const;
		// Whether a step's staging of `bytes` fits beside the staging in flight within the context's staging room
		// (AssetFinalizeContext::GetStagingRoom); the frame's first step within the whole of StagingBytes, which the others
		// leave a step of, or alone when it is larger.
		bool HasStagingRoom(const AssetFinalizeContext& context, uint64_t bytes, bool framesFirstStep) const;
		// Whether this is the frame's first step: the frame's first call, which uploaded nothing yet, in a frame that uploaded
		// nothing yet.
		static bool IsFramesFirstStep(const AssetFinalizeContext& context, uint64_t callBytes);
		// The bytes of a region's rows (of blocks).
		static uint64_t GetRegionBytes(TextureFormat format, const GPUTextureRegion& region);
		// The staging bytes a texture upload of these regions takes: the staging buffer the pool hands out for them, one after
		// the other (StagingPool::GetBufferByteSize).
		static uint64_t GetStagingBytes(TextureFormat format, std::span<const GPUTextureRegion> regions);

		// Copies the regions (runs of rows of mip levels) into the texture through one staging buffer, which the pool may hold
		// within the context's staging budget, on `queue`; adds the bytes and the time to the context's UploadedBytes and
		// UploadMs. Records nothing when no staging could be had. Within a batch. On the transfer queue the levels must be
		// written from their first row on, and handed over to the graphics queue once the upload is complete.
		GPUUploadStep UploadTexture(const AssetFinalizeContext& context, nvrhi::ITexture* texture, TextureFormat format, std::span<const GPUTextureRegion> regions,
			GPUUploadQueueKind queue);
		// Copies the ranges into their buffers through one staging buffer (their sizes added up), like UploadTexture.
		GPUUploadStep UploadBuffer(const AssetFinalizeContext& context, std::span<const GPUBufferRegion> regions, GPUUploadQueueKind queue);
		// Copies `levels` mip levels of `source` from sourceMip on into `destination` from destinationMip on, on the GPU (the
		// levels' sizes must match). On the graphics queue, within a batch.
		void CopyTextureLevels(nvrhi::ITexture* destination, uint32_t destinationMip, nvrhi::ITexture* source, uint32_t sourceMip, uint32_t levels);

		// Hands what an upload on the transfer queue wrote over to the graphics queue: records the release after its last
		// copy and returns the ticket to wait for; once it is complete, AcquireOnGraphics records the acquire, after which the
		// graphics queue may use the resource. Within a batch.
		GPUUploadTicket ReleaseToGraphics(const GPUQueueTransfer& transfer);
		void AcquireOnGraphics(const GPUQueueTransfer& transfer);

		// The ticket of what was recorded so far (complete once the GPU has run it): the open batch's, or the latest one's.
		GPUUploadTicket GetTicket() const;
		// Whether the GPU has run everything recorded before the ticket was taken (polled, never waiting). A ticket of the
		// batch that is still open is not complete.
		bool IsComplete(GPUUploadTicket ticket);
		// Blocks until the ticket is complete, submitting the open batch first when the ticket is its own (a new batch is
		// opened in its place). For finalizations that have to finish at once (AssetFinalizeContext::MayWait false).
		void WaitForTicket(GPUUploadTicket ticket);

		const GPUUploadQueueStats& GetStats() const { return m_Stats; }
	private:
		// A run of bytes a step writes into staging memory.
		struct CopyRun
		{
			uint8_t* Destination = nullptr;
			const uint8_t* Source = nullptr;
			size_t Bytes = 0;
		};
		struct Submission
		{
			GPUUploadTicket Ticket = 0;
			nvrhi::EventQueryHandle Graphics;
			nvrhi::EventQueryHandle Transfer; // Null when the batch recorded nothing on the transfer queue
			std::vector<nvrhi::TextureHandle> Textures; // What its copies wrote, kept alive until they completed
		};

		// The open batch's command list of the queue (the transfer queue's is opened on first use in a batch).
		nvrhi::ICommandList* GetList(GPUUploadQueueKind queue);
		nvrhi::EventQueryHandle SetQuery(nvrhi::CommandQueue queue);
		// Collects a step's writes into mapped staging memory; RunCopies makes them before the staging is unmapped, spread over
		// the worker threads too when they are large and `spread` (staging that was written before).
		void AddCopy(uint8_t* destination, const uint8_t* source, size_t bytes);
		void RunCopies(bool spread);
		// Adds a step's bytes and time to the context, the statistics and the measured speed.
		// Counts a step: `milliseconds` in all, of which `acquireMs` acquiring and mapping its staging (`created`: a new buffer).
		void RecordStep(const AssetFinalizeContext& context, uint64_t bytes, float milliseconds, float acquireMs, bool created, bool texture,
			GPUUploadQueueKind queue);
		// Marks the submissions whose queries completed (polled; they complete in submission order).
		void CollectCompleted();
	private:
		GraphicsDevice& m_GraphicsDevice;
		nvrhi::IDevice* m_Device;
		StagingPool& m_Staging;
		bool m_UseTransferQueue = false;

		nvrhi::CommandListHandle m_GraphicsList;
		nvrhi::CommandListHandle m_TransferList;
		bool m_BatchOpen = false;
		bool m_GraphicsOpen = false; // The open batch opened the graphics queue's list (it recorded something)
		std::optional<GPUPassTimer> m_GraphicsTimer; // The GPU's time for the open batch's graphics list (GPU.Uploads)
		bool m_TransferOpen = false; // The open batch opened the transfer queue's list
		// What the open batch's transfer list wrote: their states are given back to NVRHI's tracking before it closes, so that
		// it records no transitions into shader states, which the transfer queue cannot make.
		std::vector<nvrhi::TextureHandle> m_TransferTextures;
		std::vector<nvrhi::BufferHandle> m_TransferBuffers;
		// The textures the open batch's copies write: NVRHI's command lists do not reference the destinations of copies from
		// buffers (GraphicsDevice::RecordBufferToTextureCopies), so the batch's submission keeps them until they completed.
		std::vector<nvrhi::TextureHandle> m_BatchTextures;
		std::vector<GPUBufferTextureCopy> m_TextureCopies; // A step's copies, reused

		GPUUploadTicket m_BatchTicket = 1;     // The open (or next) batch's ticket
		GPUUploadTicket m_CompletedTicket = 0; // Every batch up to this one is complete
		std::deque<Submission> m_Submissions;  // Oldest first
		std::vector<nvrhi::EventQueryHandle> m_IdleQueries;

		std::vector<CopyRun> m_Copies; // The step's writes (AddCopy), reused
		size_t m_CopyBytes = 0;

		GPUUploadQueueStats m_Stats;
	};

}
