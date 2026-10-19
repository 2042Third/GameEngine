#pragma once

#include "Strata/Core/Base.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Strata
{

	// A stage of the frame (FrameProfiler::RegisterStage); ids count up from 0 in registration order.
	using FrameStageID = uint32_t;
	constexpr FrameStageID c_InvalidFrameStage = ~FrameStageID(0);

	// Where a stage's time is spent. CPU stages are timed on the frame thread (ST_PROFILE_STAGE); GPU stages are the GPU's
	// time for passes of the frame, measured by the renderer (GPUProfiler) and attached to the frame frames later
	// (FrameProfiler::RecordGPUFrame). GPU stages are never part of the frame's work time.
	enum class FrameStageKind : uint8_t
	{
		CPU = 0,
		GPU
	};

	// A GPU stage's time in a frame (FrameProfiler::RecordGPUFrame).
	struct GPUStageTime
	{
		FrameStageID Stage = c_InvalidFrameStage;
		float Milliseconds = 0.0f;
	};

	// When a frame listener hears of a frame (FrameProfiler::AddFrameListener).
	enum class FrameListenerTiming : uint8_t
	{
		Immediate = 0, // When the frame ends, before its GPU times can be known
		// Once the frame's GPU times are attached (or known not to come), in frame order: after FrameProfiler::c_MaxGPULatencyFrames
		// later frames without them at the latest. Right away while no GPU times are expected (SetGPUTimesExpected).
		WithGPU
	};

	// What paces the frames (FrameGPUSummary::BoundBy).
	enum class FrameBound : uint8_t
	{
		Unknown = 0, // No GPU times
		CPU,         // The median work time takes at least 90% of the median frame interval
		GPU,         // The median GPU busy time takes at least 90% of the median interval and exceeds the median work time
		Capped       // Neither: vsync, a frame-rate cap or idle throttling sets the pace
	};

	// A stage every FrameProfiler registers first, in this order (so their ids are their positions).
	struct BuiltinFrameStage
	{
		std::string_view Name;
		// Whether the stage lies inside the frame's work time (Application::GetLastFrameWorkTime). Present runs after the
		// work time is measured: it may wait for the display, which is no work of the frame.
		bool InWorkTime = true;
	};

	inline constexpr std::array<BuiltinFrameStage, 12> c_BuiltinFrameStages = { {
		{ "Input", true },            // Input::BeginFrame and the window's events (Application::RunFrame)
		{ "Assets.Update", true },    // AssetManagerBase::Update (and the editor's file changes and imports) without finalization
		{ "Assets.Finalize", true },  // Finalizing loaded assets on the main thread: GPU uploads (AssetManagerBase)
		{ "Scene.Load", true },       // Scene loads' main-thread work: swapping a loaded scene in, creating sub-scenes' entities
		{ "Scene.Scripts", true },    // The "Scripting" scene system's callbacks (SceneSystemDescriptor::ProfileStage)
		{ "Scene.Physics", true },    // The "Physics" scene system: stepping and contact callbacks
		{ "Scene.Audio", true },      // The "Audio" scene system
		{ "Scene.Transforms", true }, // Scene::UpdateWorldTransforms
		{ "Render.Extract", true },   // SceneRenderer gathering lights, draws and shadow casters from the scene
		{ "Render.Submit", true },    // SceneRenderer recording and submitting its command list
		{ "Editor.UI", true },        // ImGui: the layers' OnImGuiRender and drawing the UI (ImGuiModule::OnRenderUI)
		{ "Present", false }          // GraphicsDevice::EndFrame (presenting the back buffer), after the work time
	} };

	// Statistics of per-frame times in milliseconds: the mean, the 50th and 95th percentiles by nearest rank (the smallest
	// sample that at least that share of the samples does not exceed) and the maximum. All zero without samples.
	struct FrameTimeStats
	{
		float Average = 0.0f;
		float P50 = 0.0f;
		float P95 = 0.0f;
		float Max = 0.0f;
	};

	// One recorded frame. StageMs is valid until the profiler records its next frame (or is reset).
	struct FrameSample
	{
		uint64_t Index = 0;              // Frames recorded before this one since the profiler started (or was reset)
		float WorkMs = 0.0f;             // The frame's work time, as EndFrame was given it
		float IntervalMs = 0.0f;         // From the previous frame's BeginFrame to this one's (0 for the first)
		float WorkerBusyPercent = 0.0f;  // How busy the job system's worker threads were since the previous frame ended
		float IOBusyPercent = 0.0f;      // Its I/O threads, likewise (0 without I/O threads)
		float BackgroundBusyPercent = 0.0f; // Its background threads, likewise
		std::span<const float> StageMs;  // Self time per stage id (nested stages excluded), one entry per registered stage (0 for GPU stages)
		// The GPU's time (FrameProfiler::RecordGPUFrame): whether it was attached, the GPU stages added up, and per stage id
		// (one entry per registered stage, 0 for CPU stages and while HasGPU is false).
		bool HasGPU = false;
		float GPUBusyMs = 0.0f;
		std::span<const float> GPUStageMs;
		uint32_t GPULatencyFrames = 0; // Frames recorded after this one by the time its GPU times arrived
	};

	struct FrameStageSummary
	{
		std::string Name;
		bool InWorkTime = true;
		FrameStageKind Kind = FrameStageKind::CPU;
		FrameTimeStats Time; // GPU stages: over the frames with GPU times only
	};

	// The GPU's share of the summarized frames (FrameProfileSummary::GPU).
	struct FrameGPUSummary
	{
		uint32_t Frames = 0;          // Frames with GPU times
		FrameTimeStats Busy;          // Their GPU stages added up per frame
		float LatencyFrames = 0.0f;   // Average frames between a frame and the arrival of its GPU times
		FrameBound BoundBy = FrameBound::Unknown;
	};

	// Statistics of the latest frames (FrameProfiler::Summarize).
	struct FrameProfileSummary
	{
		uint32_t Frames = 0;
		FrameTimeStats Work;        // The frames' work time
		FrameTimeStats StageSum;    // Per frame, the stages inside the work time added up (at most the work time)
		FrameTimeStats Untimed;     // Per frame, the work time no stage accounts for (the work time minus StageSum)
		FrameTimeStats Interval;    // The time from each frame's start to the next one's (frames that have a previous frame)
		float FramesPerSecond = 0.0f; // Frames per second of the intervals between them (0 with fewer than two frames)
		float WorkerBusyPercent = 0.0f;
		float IOBusyPercent = 0.0f;
		float BackgroundBusyPercent = 0.0f;
		uint32_t WorkerThreads = 0; // The job system's threads when summarized
		uint32_t IOThreads = 0;
		uint32_t BackgroundThreads = 0;
		std::vector<FrameStageSummary> Stages; // Every registered stage, in id order
		FrameGPUSummary GPU;
	};

	struct FrameProfilerSpecification
	{
		uint32_t Capacity = 600; // Frames kept (at least 1); older ones are overwritten
		// A monotonic clock in nanoseconds; null for the steady clock. Tests drive the profiler with a clock of their own.
		std::function<int64_t()> Clock;
		// Sample the job system's counters at the end of each frame for the busy percentages (JobSystem::GetStats).
		bool SampleJobs = true;
	};

	// An always-on timer of the main thread's frame stages. The thread that drives the frames (BeginFrame and EndFrame,
	// Application::RunFrame) times stages with ST_PROFILE_STAGE scopes; scopes on other threads, and outside a frame, do
	// nothing. Stages nest: each records its self time (the time of the stages nested in it excluded), so the stages of a
	// frame never add up to more than the frame, and a stage entered several times in a frame (the scene renderer of the
	// viewport and of a capture) adds up. The last Capacity frames are kept in a ring.
	//
	// RegisterStage may be called from any thread; everything else from the thread that drives the frames (one thread does,
	// and calling BeginFrame from another makes that one the frame thread).
	class FrameProfiler
	{
	public:
		static constexpr uint32_t c_DefaultCapacity = 600;
		static constexpr uint32_t c_MaxStages = 64;
		// Stages nested deeper are not timed (their time counts for the stage around them).
		static constexpr uint32_t c_MaxDepth = 32;
		// WithGPU listeners hear of a frame without GPU times once this many later frames were recorded.
		static constexpr uint32_t c_MaxGPULatencyFrames = 8;

		explicit FrameProfiler(const FrameProfilerSpecification& specification = {});

		FrameProfiler(const FrameProfiler&) = delete;
		FrameProfiler& operator=(const FrameProfiler&) = delete;

		// The process-wide profiler: the one ST_PROFILE_STAGE, the applications and editor.perf use.
		static FrameProfiler& Get();

		// The id of the stage with this name, registered now if it is new. c_InvalidFrameStage (with a warning, once) when
		// c_MaxStages stages are registered already: that stage is then not timed. GPU stages are never in the work time. A
		// name registered before keeps its kind (asserted). Any thread.
		FrameStageID RegisterStage(std::string_view name, bool inWorkTime = true, FrameStageKind kind = FrameStageKind::CPU);
		// c_InvalidFrameStage when no stage has the name. Any thread.
		FrameStageID FindStage(std::string_view name) const;
		uint32_t GetStageCount() const { return m_StageCount.load(std::memory_order_acquire); }
		// The name of a registered stage (empty for any other id). The reference stays valid as long as the profiler.
		const std::string& GetStageName(FrameStageID stage) const;
		bool IsStageInWorkTime(FrameStageID stage) const;
		FrameStageKind GetStageKind(FrameStageID stage) const;

		// Starts a frame on the calling thread, which becomes the frame thread. A frame that was begun but not ended (the
		// application skipped it) is discarded.
		void BeginFrame();
		// Ends the frame and records it with its work time (Application::GetLastFrameWorkTime: the frame without the waits for
		// the GPU and the display), then calls the frame listeners. Stages still open continue into the next frame.
		void EndFrame(double workSeconds);
		bool IsInFrame() const { return m_InFrame.load(std::memory_order_relaxed); }
		// Whether stages entered on the calling thread are timed now: it is the frame thread, inside a frame. Any thread;
		// cheap, so that code calling many short stages can skip them all at once (stages check it themselves).
		bool IsTimingThisThread() const { return IsInFrame() && IsFrameThread(); }

		// Used by FrameStageScope. Begin returns whether the stage was entered (the frame thread, inside a frame, a registered
		// CPU stage, within c_MaxDepth); only then does End have to follow. End leaves the innermost stage.
		bool BeginStage(FrameStageID stage);
		void EndStage();

		uint32_t GetCapacity() const { return m_Capacity; }
		// Frames in the ring (at most the capacity).
		uint32_t GetRecordedFrameCount() const { return m_RecordedFrames; }
		// Frames recorded since the profiler started or was reset.
		uint64_t GetTotalFrameCount() const { return m_TotalFrames; }
		// A recorded frame: age 0 is the latest. Requires age < GetRecordedFrameCount().
		FrameSample GetFrame(uint32_t age) const;
		// Statistics of the latest `frames` frames (fewer when fewer were recorded), and of every registered stage.
		FrameProfileSummary Summarize(uint32_t frames) const;
		// Statistics of samples in milliseconds, in any order (exact: the percentiles are samples).
		static FrameTimeStats ComputeStats(std::span<const float> samples);
		// What paces frames of these median work, GPU busy and interval times (FrameBound); Unknown without an interval.
		static FrameBound ClassifyFrameBound(float workMs, float gpuBusyMs, float intervalMs);
		// The share of the frame interval a bound takes (FrameBound).
		static constexpr float c_BoundShare = 0.9f;

		// Attaches GPU times to a recorded frame (`frameIndex`: FrameSample::Index) while it is still in the ring: its GPU stages'
		// times (a stage listed twice adds up), or none for a frame that had no GPU work timed. False when the frame is not in
		// the ring any more (or not recorded yet). The frame thread.
		bool RecordGPUFrame(uint64_t frameIndex, std::span<const GPUStageTime> stages);
		// Whether GPU times are expected for the frames (a GPUProfiler measures them): WithGPU listeners wait for them only
		// then. Off by default. The frame thread.
		void SetGPUTimesExpected(bool expected);
		bool AreGPUTimesExpected() const { return m_GPUTimesExpected; }
		// Calls the WithGPU listeners for every recorded frame they have not heard of yet, GPU times or not (the end of a run).
		void FlushGPUListeners();

		// Called on the frame thread after each recorded frame (Immediate) or once its GPU times are known (WithGPU, see
		// FrameListenerTiming). Listeners must not add or remove listeners while called.
		uint32_t AddFrameListener(std::function<void(const FrameSample&)> listener, FrameListenerTiming timing = FrameListenerTiming::Immediate);
		void RemoveFrameListener(uint32_t id);

		// Forgets the recorded frames (stages and listeners stay). GPU times recorded for frames before the reset are refused.
		void Reset();
		// How often Reset was called: GPU times are attached only to frames of the same reset count as their recording.
		uint32_t GetResetCount() const { return m_ResetCount; }
	private:
		struct OpenStage
		{
			FrameStageID Stage = c_InvalidFrameStage;
			int64_t Start = 0;
		};

		struct FrameRecord
		{
			uint64_t Index = 0;
			float WorkMs = 0.0f;
			float IntervalMs = 0.0f;
			float WorkerBusyPercent = 0.0f;
			float IOBusyPercent = 0.0f;
			float BackgroundBusyPercent = 0.0f;
			std::array<float, c_MaxStages> StageMs = {};
			bool GPUResolved = false; // GPU times arrived (or are known not to come: RecordGPUFrame without stages)
			bool HasGPU = false;
			float GPUBusyMs = 0.0f;
			uint32_t GPULatencyFrames = 0;
			std::array<float, c_MaxStages> GPUStageMs = {};
		};

		struct Listener
		{
			uint32_t ID = 0;
			FrameListenerTiming Timing = FrameListenerTiming::Immediate;
			std::function<void(const FrameSample&)> Callback;
		};

		struct JobSample
		{
			bool Valid = false;
			int64_t Time = 0;
			int64_t WorkerBusy = 0;
			int64_t IOBusy = 0;
			int64_t BackgroundBusy = 0;
			uint32_t WorkerThreads = 0;
			uint32_t IOThreads = 0;
			uint32_t BackgroundThreads = 0;
		};

		int64_t Now() const;
		bool IsFrameThread() const { return std::this_thread::get_id() == m_FrameThread.load(std::memory_order_relaxed); }
		JobSample SampleJobs() const;
		FrameSample ToSample(const FrameRecord& record) const;
		// The record of a frame still in the ring, or null.
		FrameRecord* FindRecord(uint64_t frameIndex);
		// Calls the WithGPU listeners for the frames they have not heard of, in order, while each one's GPU times are known,
		// not expected, or late by c_MaxGPULatencyFrames frames, and in any case for the frames before `forcedEnd`.
		void DeliverGPUListeners(uint64_t forcedEnd);
		bool HasGPUListeners() const;
	private:
		FrameProfilerSpecification m_Specification;
		uint32_t m_Capacity = c_DefaultCapacity;

		// Stages: names are written once, before the count that publishes them.
		mutable std::mutex m_StageMutex;
		std::array<std::string, c_MaxStages> m_StageNames;
		std::array<bool, c_MaxStages> m_StageInWorkTime = {};
		std::array<FrameStageKind, c_MaxStages> m_StageKinds = {};
		std::atomic<uint32_t> m_StageCount = 0;
		bool m_WarnedStageLimit = false; // Guarded by m_StageMutex

		// The frame in progress (the frame thread only, but for the two atomics, which any thread may read: stages entered on
		// other threads or outside frames check them and return).
		std::atomic<std::thread::id> m_FrameThread;
		std::atomic<bool> m_InFrame = false;
		int64_t m_FrameStart = 0;
		int64_t m_PreviousFrameStart = 0;
		bool m_HasPreviousFrame = false;
		std::array<int64_t, c_MaxStages> m_Accumulated = {};
		std::array<OpenStage, c_MaxDepth> m_Open = {};
		uint32_t m_Depth = 0;
		JobSample m_LastJobSample;

		// The ring of recorded frames (the frame thread only).
		std::vector<FrameRecord> m_Records;
		uint32_t m_NextRecord = 0;
		uint32_t m_RecordedFrames = 0;
		uint64_t m_TotalFrames = 0;

		std::vector<Listener> m_Listeners;
		uint32_t m_NextListenerID = 1;
		uint64_t m_NextGPUListenerFrame = 0; // The first frame the WithGPU listeners have not heard of
		bool m_GPUTimesExpected = false;
		uint32_t m_ResetCount = 0;
	};

	// Times a stage of the frame for as long as it lives (see FrameProfiler; ST_PROFILE_STAGE makes one).
	class FrameStageScope
	{
	public:
		FrameStageScope(FrameProfiler& profiler, FrameStageID stage)
			: m_Profiler(profiler), m_Active(profiler.BeginStage(stage))
		{
		}

		~FrameStageScope()
		{
			if (m_Active)
				m_Profiler.EndStage();
		}

		FrameStageScope(const FrameStageScope&) = delete;
		FrameStageScope& operator=(const FrameStageScope&) = delete;
	private:
		FrameProfiler& m_Profiler;
		bool m_Active = false;
	};

}
