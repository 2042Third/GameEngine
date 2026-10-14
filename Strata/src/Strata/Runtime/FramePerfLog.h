#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/FrameProfiler.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace Strata
{

	struct AssetManagerStats;
	struct GPUUploadQueueStats;

	// The graphics device's memory after a frame (FramePerfLog::WriteFrame): what the process has allocated on the device
	// (GraphicsDevice::GetMemoryBudget().Usage: textures, buffers, render targets, the swapchain, staging) and, of it, the
	// upload staging the renderer's pool holds (StagingPool: idle, and waiting for the GPU's copies).
	struct FramePerfDeviceMemory
	{
		uint64_t DeviceBytes = 0;
		uint64_t StagingBytes = 0;
	};

	// A CSV file with one row per frame (StrataRuntime --perf-log): what the frame profiler recorded and where the frame left
	// asset streaming, for CI and tools to read. The header names the columns:
	//   Frame, WorkMs, IntervalMs                the frame (FrameSample: index, work time, time since the previous frame)
	//   <stage>...                               each stage the profiler had registered when the log was created (the
	//                                            built-in ones first: Input, Assets.Update, ...; Create registers every
	//                                            registered scene system's stage first), in milliseconds; GPU stages
	//                                            (GPU.Shadows...) are the GPU's time, empty in a row without GPU times
	//   WorkerBusyPercent, IOBusyPercent         the job system's threads
	//   LoadingAssets, QueuedLoads, InFlightLoads, InFlightMB, UploadedMB, FinalizeMs, UploadMs, ResidentGpuTexturesMB,
	//   ResidentGpuBuffersMB, ResidentCpuMB, Evictions, StreamedMB, MipLevelsRaised, MipLevelsDropped, MipsBelowWanted
	//                                            the asset manager after the frame (AssetManagerStats; MB are 2^20 bytes,
	//                                            empty without an asset manager); the last four are its texture streamer's
	//                                            (AssetManagerStats::Detail): the frame's bytes read for raised mip levels,
	//                                            the levels raised and dropped so far, and the textures below the level
	//                                            their views need
	//   UploadQueueMB, UploadQueueMs, UploadQueueSteps
	//                                            the frame's steps through the renderer's upload queue: bytes, CPU time and
	//                                            count (GPUUploadQueueStats; empty without a renderer)
	//   DeviceMB, StagingMB                      the graphics device's memory in use and the staging of it
	//                                            (FramePerfDeviceMemory; empty without a renderer): with the resident pools
	//                                            above they tell what render targets and the swapchain take
	//   PrivateMB, PeakPrivateMB                 the process's private memory after the frame and its peak so far
	//                                            (Platform::GetProcessMemory; empty where the system does not report it)
	//   GPUFrame, GPUBusyMs                      1 when the frame's GPU times arrived (else 0), and its GPU stages added up
	//                                            (empty without them)
	// A row describes one frame: its numbers beside the profiler's are captured when the frame ends (CaptureFrame), and the
	// row is written once the frame's GPU times are known (WriteFrame from a FrameListenerTiming::WithGPU listener). The
	// file is complete once the log is closed.
	class FramePerfLog
	{
	public:
		// Creates (or replaces) the file and writes the header, after registering the stage of every registered scene system
		// in the profiler (SceneSystemDescriptor::ProfileStage; scenes register them when they first run, which may be after
		// the header), so that every system has a column. Null with the reason in outError when it cannot be written.
		static Scope<FramePerfLog> Create(const std::filesystem::path& path, FrameProfiler& profiler, std::string* outError = nullptr);
		~FramePerfLog();

		FramePerfLog(const FramePerfLog&) = delete;
		FramePerfLog& operator=(const FramePerfLog&) = delete;

		// Captures a frame's numbers beside the profiler's when it ends (an Immediate frame listener calls it): the asset
		// manager's statistics after the frame, the upload queue's and the device's memory before the next frame begins (null:
		// none) and the process's memory, for the frame's row.
		void CaptureFrame(const FrameSample& frame, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads = nullptr,
			const FramePerfDeviceMemory* device = nullptr);
		// Appends the row of a recorded frame once its GPU times are known (a WithGPU frame listener calls it), with what
		// CaptureFrame captured for it (those columns are empty when nothing was). False once writing failed; later rows are
		// not written.
		bool WriteFrame(const FrameSample& frame);
		// Captures and writes a row at once (a frame whose GPU times are not awaited).
		bool WriteFrame(const FrameSample& frame, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads = nullptr,
			const FramePerfDeviceMemory* device = nullptr);
		// Flushes and closes the file. False with the reason when writing failed at any point.
		bool Close(std::string* outError = nullptr);

		const std::filesystem::path& GetPath() const { return m_Path; }
		uint64_t GetRowCount() const { return m_Rows; }
		const std::vector<std::string>& GetColumns() const { return m_Columns; }
	private:
		// The columns CaptureFrame fills of a frame, formatted.
		struct CapturedFrame
		{
			uint64_t Index = 0;
			std::string Columns;
		};

		FramePerfLog() = default;
		static void FormatCapture(std::string& out, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads, const FramePerfDeviceMemory* device);
	private:
		std::filesystem::path m_Path;
		const FrameProfiler* m_Profiler = nullptr; // Outlives the log
		std::ofstream m_File;
		std::vector<std::string> m_Columns;
		uint32_t m_StageCount = 0; // Stage columns: the stages registered when the log was created
		std::vector<bool> m_GPUStages; // Per stage column: a GPU stage
		uint32_t m_WarnedStages = 0;   // Stages registered after the header that were warned about (by count)
		std::deque<CapturedFrame> m_Captured; // Oldest first
		uint64_t m_Rows = 0;
		bool m_Failed = false;
		bool m_Closed = false;
		std::string m_Line; // Reused for each row
	};

}
