#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/FrameProfiler.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
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

	// The values a column group writes into a row (FramePerfColumnGroup::Write): one call of Add or AddEmpty per column it
	// names, in their order.
	class FramePerfRow
	{
	public:
		explicit FramePerfRow(std::string& line)
			: m_Line(line)
		{
		}

		// A number with `decimals` digits after the point (0-9); an empty field for a value that is no finite number.
		void Add(double value, int decimals);
		// An empty field: nothing to report this frame (no world streams, say).
		void AddEmpty();
		uint32_t GetCount() const { return m_Count; }
	private:
		std::string& m_Line;
		uint32_t m_Count = 0;
	};

	// Columns a program adds after the built-in ones (FramePerfLogSpecification): their names, and a function that writes
	// their values for each row, read when the row is written (after the frame).
	struct FramePerfColumnGroup
	{
		std::vector<std::string> Names;
		std::function<void(FramePerfRow&)> Write; // Null: the group's fields stay empty
	};

	struct FramePerfLogSpecification
	{
		// Groups of columns that follow every built-in column, in this order. A group that writes another number of values
		// than it names fails the log like a write error.
		std::vector<FramePerfColumnGroup> Columns;
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
	//   <group columns>...                       the columns of the specification's groups, in their order (empty in a row
	//                                            whose frame was not captured)
	// A row describes one frame: its numbers beside the profiler's (and the groups' values) are captured when the frame ends
	// (CaptureFrame), and the row is written once the frame's GPU times are known (WriteFrame from a
	// FrameListenerTiming::WithGPU listener). The file is complete once the log is closed.
	class FramePerfLog
	{
	public:
		// Creates (or replaces) the file and writes the header, after registering the stage of every registered scene system
		// in the profiler (SceneSystemDescriptor::ProfileStage; scenes register them when they first run, which may be after
		// the header), so that every system has a column. Null with the reason in outError when it cannot be written, or
		// when a group names no column or names one twice.
		static Scope<FramePerfLog> Create(const std::filesystem::path& path, FrameProfiler& profiler,
			const FramePerfLogSpecification& specification = {}, std::string* outError = nullptr);
		~FramePerfLog();

		FramePerfLog(const FramePerfLog&) = delete;
		FramePerfLog& operator=(const FramePerfLog&) = delete;

		// Captures a frame's numbers beside the profiler's when it ends (an Immediate frame listener calls it): the asset
		// manager's statistics after the frame, the upload queue's and the device's memory before the next frame begins (null:
		// none), the process's memory and the column groups' values, for the frame's row. A group that writes another count
		// of values than it names fails the log.
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
			std::string Groups; // The column groups' fields
		};

		FramePerfLog() = default;
		static void FormatCapture(std::string& out, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads, const FramePerfDeviceMemory* device);
		// Appends the column groups' fields; false (the log failed) when a group wrote another count of values than it names.
		bool FormatGroups(std::string& out);
	private:
		std::filesystem::path m_Path;
		const FrameProfiler* m_Profiler = nullptr; // Outlives the log
		std::ofstream m_File;
		std::vector<std::string> m_Columns;
		uint32_t m_StageCount = 0; // Stage columns: the stages registered when the log was created
		std::vector<bool> m_GPUStages; // Per stage column: a GPU stage
		uint32_t m_WarnedStages = 0;   // Stages registered after the header that were warned about (by count)
		std::deque<CapturedFrame> m_Captured; // Oldest first
		std::vector<FramePerfColumnGroup> m_Groups;
		size_t m_GroupColumnCount = 0;
		uint64_t m_Rows = 0;
		bool m_Failed = false;
		bool m_Closed = false;
		std::string m_Line; // Reused for each row
	};

}
