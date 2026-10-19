#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/FrameProfiler.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <vector>

namespace Strata
{

	class GraphicsDevice;
	class GPUTimestampPool;

	// The renderer's GPU stages (FrameStageKind::GPU), ids of FrameProfiler::Get(). RegisterRendererModule registers them, so
	// they exist before anything reads the profiler's stages (the perf log's columns); Get registers them on first use too.
	struct RendererGPUStages
	{
		FrameStageID Uploads = c_InvalidFrameStage;     // The upload queue's graphics batches (GPUUploadQueue::SubmitBatch)
		FrameStageID Setup = c_InvalidFrameStage;       // Scene data uploads, environment maps, light clusters
		FrameStageID Cull = c_InvalidFrameStage;        // Culling dispatches and depth pyramids
		FrameStageID Shadows = c_InvalidFrameStage;
		FrameStageID Prepass = c_InvalidFrameStage;
		FrameStageID AO = c_InvalidFrameStage;
		FrameStageID Opaque = c_InvalidFrameStage;      // With the sky
		FrameStageID Transparent = c_InvalidFrameStage;
		FrameStageID Post = c_InvalidFrameStage;        // Exposure, bloom, tone mapping, FXAA
		FrameStageID Overlays = c_InvalidFrameStage;    // Grid, tints, outline, lines, text, the copy into a target

		static const RendererGPUStages& Get();
	};

	// An interval a GPUPassTimer timed: a stage between two timestamps of one frame slot of the profiler.
	struct GPUTimedInterval
	{
		FrameStageID Stage = c_InvalidFrameStage;
		uint32_t Begin = 0; // Timestamps of the pool
		uint32_t End = 0;
	};

	// The intervals one user (a scene renderer) timed in one frame, for reading its own GPU times (GPUProfiler::ReadRecord)
	// besides the frame's sums the profiler hands to the frame profiler.
	struct GPUTimerRecord
	{
		uint64_t Slot = 0;          // The serial of the profiler's frame slot (0: none)
		uint64_t ProfilerFrame = 0; // The frame profiler's frame it belongs to (FrameSample::Index)
		std::vector<GPUTimedInterval> Intervals;

		void Clear()
		{
			Slot = 0;
			ProfilerFrame = 0;
			Intervals.clear();
		}
	};

	struct GPUProfilerStats
	{
		uint64_t ResolvedFrames = 0;    // Frames whose GPU times were handed to the frame profiler
		// Frames whose GPU times were lost: still pending when their slot's turn to be reused came, or with timestamps that
		// were never written (a command list that was not executed); a frame that found its slot still in use by the GPU.
		uint64_t DroppedFrames = 0;
		uint64_t SkippedTimestamps = 0; // Timestamps beyond a frame's c_MaxTimestampsPerFrame, not written
		uint64_t Timestamps = 0;        // Timestamps written
		// The profiler's own CPU time (its writes, resolves and hand-offs), accumulated since it was created: what timing the
		// GPU costs the frame (PerfGPU.Renderer.GPUTimerCost).
		double CPUMilliseconds = 0.0;
	};

	// Measures the GPU's time per stage of the frame with timestamps (GraphicsDevice::CreateTimestampPool) and hands it to the
	// frame profiler, frames later, without ever waiting for the GPU. A ring of MaxFramesInFlight + 2 frame slots of
	// c_MaxTimestampsPerFrame timestamps: BeginFrame (Renderer::BeginFrame) closes the frame's slot, reads the oldest pending
	// slots that the GPU completed and hands their per-stage sums (every user's intervals added up, as CPU stages add up) to
	// the frame profiler for the frame that recorded them (FrameProfiler::RecordGPUFrame), then opens the next slot. A slot
	// still in use when its turn comes is dropped, never waited for, and so is one whose timestamps are still not readable a
	// frame after the GPU completed it (a timed command list that was never executed; drivers may make the results readable a
	// little after the work completed, as MoltenVK does). Passes are timed with GPUPassTimer. Without timestamp support
	// everything is a no-op. Main thread only.
	class GPUProfiler
	{
	public:
		static constexpr uint32_t c_MaxTimestampsPerFrame = 256;
		static constexpr uint32_t c_NoTimestamp = ~0u;

		explicit GPUProfiler(GraphicsDevice& device, FrameProfiler& profiler = FrameProfiler::Get());
		~GPUProfiler();

		GPUProfiler(const GPUProfiler&) = delete;
		GPUProfiler& operator=(const GPUProfiler&) = delete;

		// Whether the device writes timestamps (and the pool exists).
		bool IsEnabled() const { return m_Pool != nullptr; }
		uint32_t GetSlotCount() const { return static_cast<uint32_t>(m_Slots.size()); }

		// Closes the frame's slot, resolves what the GPU completed and opens the next frame's slot (Renderer::BeginFrame).
		void BeginFrame();
		// Resolves the pending slots the GPU completed; with `wait`, waits for the GPU first (tests and tools), which also
		// resolves the open slot's completed intervals for ReadRecord.
		void Resolve(bool wait);

		// Used by GPUPassTimer: writes a timestamp into the open slot (c_NoTimestamp when there is none, or it is full), and
		// adds an interval between two of its timestamps to the slot (and to `record`).
		uint32_t WriteTimestamp(nvrhi::ICommandList* commandList);
		void AddInterval(FrameStageID stage, uint32_t begin, uint32_t end, GPUTimerRecord* record);
		// The serial of the open slot (0 without one).
		uint64_t GetOpenSlot() const;

		// A user's own GPU times: its intervals summed per stage into `outStages` (cleared first) once every timestamp of the
		// record is written. False while one is not, or once the record's slot was reused.
		bool ReadRecord(const GPUTimerRecord& record, std::vector<GPUStageTime>& outStages);

		const GPUProfilerStats& GetStats() const { return m_Stats; }
	private:
		enum class SlotState : uint8_t
		{
			Free = 0,
			Open,    // The frame's timestamps are being written
			Pending  // Closed, waiting for the GPU
		};

		struct Slot
		{
			SlotState State = SlotState::Free;
			uint64_t Serial = 0;
			uint64_t ProfilerFrame = 0;
			uint32_t ResetCount = 0;  // The frame profiler's, when the slot was opened
			uint32_t Count = 0;       // Timestamps written
			uint32_t CompletedCount = 0; // Of the open slot's timestamps, those the GPU ran by the latest Resolve(true)
			std::vector<GPUTimedInterval> Intervals;
			nvrhi::EventQueryHandle Done; // Set when the slot closes: the GPU completed every submission of its frame
			bool DoneSet = false;
			// Found completed with timestamps not readable yet, in the frame profiler's frame UnreadableFrame.
			bool Unreadable = false;
			uint64_t UnreadableFrame = 0;
		};

		uint32_t GetBase(uint32_t slot) const { return slot * c_MaxTimestampsPerFrame; }
		void CloseOpenSlot();
		void OpenSlot();
		void ResolvePending();
		bool IsSlotDone(Slot& slot);
		// Hands a slot's per-stage sums to the frame profiler; false when its timestamps are not all written.
		bool ResolveSlot(Slot& slot, uint32_t index);
		void FreeSlot(Slot& slot);
		float ToMilliseconds(uint64_t begin, uint64_t end) const;
	private:
		GraphicsDevice& m_Device;
		FrameProfiler& m_Profiler;
		Scope<GPUTimestampPool> m_Pool;
		double m_PeriodNs = 0.0;
		std::vector<Slot> m_Slots;
		uint32_t m_OpenSlot = ~0u;  // Index of the open slot, or ~0u
		uint32_t m_NextSlot = 0;    // The slot the next frame opens
		uint64_t m_NextSerial = 1;
		std::vector<uint64_t> m_Ticks;  // Read scratch
		std::array<float, FrameProfiler::c_MaxStages> m_StageSums = {};
		std::vector<GPUStageTime> m_StageTimes; // Hand-off scratch
		GPUProfilerStats m_Stats;
	};

	// Times consecutive GPU passes of a command list: one timestamp per pass boundary, so the stages tile the command list
	// without gaps. Construct it where the first pass starts (`stage`), call Next where the next one starts, and End (or let
	// it go out of scope) where the last one ends, before the command list is closed. The command list must be executed in
	// the frame it was recorded in. `record` collects the intervals too (a renderer's own times). Uses the renderer's profiler
	// (Renderer::GetGPUProfiler); does nothing without one, or without timestamp support.
	class GPUPassTimer
	{
	public:
		GPUPassTimer(nvrhi::ICommandList* commandList, FrameStageID stage, GPUTimerRecord* record = nullptr);
		GPUPassTimer(GPUProfiler* profiler, nvrhi::ICommandList* commandList, FrameStageID stage, GPUTimerRecord* record = nullptr);
		~GPUPassTimer() { End(); }

		GPUPassTimer(const GPUPassTimer&) = delete;
		GPUPassTimer& operator=(const GPUPassTimer&) = delete;

		// Ends the current stage and starts `stage` (the same timestamp ends one and starts the other).
		void Next(FrameStageID stage);
		// Ends the current stage; the timer then does nothing more.
		void End();
		FrameStageID GetStage() const { return m_Stage; }
	private:
		GPUProfiler* m_Profiler = nullptr;
		nvrhi::ICommandList* m_CommandList = nullptr;
		GPUTimerRecord* m_Record = nullptr;
		FrameStageID m_Stage = c_InvalidFrameStage;
		uint32_t m_Begin = GPUProfiler::c_NoTimestamp;
		uint64_t m_Slot = 0;
	};

}
