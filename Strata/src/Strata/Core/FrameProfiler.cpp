#include "stpch.h"
#include "Strata/Core/FrameProfiler.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Log.h"

#include <algorithm>
#include <chrono>

namespace Strata
{

	namespace
	{

		constexpr double c_NanosecondsPerMillisecond = 1.0e6;
		constexpr double c_NanosecondsPerSecond = 1.0e9;

		float ToMilliseconds(int64_t nanoseconds)
		{
			return static_cast<float>(static_cast<double>(nanoseconds) / c_NanosecondsPerMillisecond);
		}

		// The share of `threads` threads busy for `busy` over `elapsed`, in percent. Each thread's counter is read at its own
		// moment just after the sample time, so a job ending in between can count a little beyond the interval: clamped.
		float ToBusyPercent(int64_t busy, int64_t elapsed, uint32_t threads)
		{
			if (elapsed <= 0 || threads == 0)
				return 0.0f;
			const double percent = 100.0 * static_cast<double>(busy) / (static_cast<double>(elapsed) * static_cast<double>(threads));
			return static_cast<float>(std::clamp(percent, 0.0, 100.0));
		}

	}

	FrameProfiler::FrameProfiler(const FrameProfilerSpecification& specification)
		: m_Specification(specification), m_Capacity(std::max(specification.Capacity, 1u))
	{
		m_Records.resize(m_Capacity);
		for (const BuiltinFrameStage& stage : c_BuiltinFrameStages)
			RegisterStage(stage.Name, stage.InWorkTime);
	}

	FrameProfiler& FrameProfiler::Get()
	{
		static FrameProfiler s_Instance;
		return s_Instance;
	}

	FrameStageID FrameProfiler::RegisterStage(std::string_view name, bool inWorkTime, FrameStageKind kind)
	{
		std::scoped_lock<std::mutex> lock(m_StageMutex);
		const uint32_t count = m_StageCount.load(std::memory_order_relaxed);
		for (uint32_t index = 0; index < count; index++)
		{
			if (m_StageNames[index] == name)
			{
				ST_CORE_ASSERT(m_StageKinds[index] == kind, "FrameProfiler: stage '{}' registered again as another kind", name);
				return index;
			}
		}
		if (count == c_MaxStages)
		{
			if (!m_WarnedStageLimit)
			{
				m_WarnedStageLimit = true;
				ST_CORE_WARN("FrameProfiler: {} stages are registered already; '{}' and later stages are not timed", c_MaxStages, name);
			}
			return c_InvalidFrameStage;
		}
		m_StageNames[count] = std::string(name);
		m_StageInWorkTime[count] = inWorkTime && kind == FrameStageKind::CPU;
		m_StageKinds[count] = kind;
		// Readers that see the new count see the name written before it.
		m_StageCount.store(count + 1, std::memory_order_release);
		return count;
	}

	FrameStageID FrameProfiler::FindStage(std::string_view name) const
	{
		std::scoped_lock<std::mutex> lock(m_StageMutex);
		const uint32_t count = m_StageCount.load(std::memory_order_relaxed);
		for (uint32_t index = 0; index < count; index++)
		{
			if (m_StageNames[index] == name)
				return index;
		}
		return c_InvalidFrameStage;
	}

	const std::string& FrameProfiler::GetStageName(FrameStageID stage) const
	{
		static const std::string s_None;
		return stage < GetStageCount() ? m_StageNames[stage] : s_None;
	}

	bool FrameProfiler::IsStageInWorkTime(FrameStageID stage) const
	{
		return stage < GetStageCount() && m_StageInWorkTime[stage];
	}

	FrameStageKind FrameProfiler::GetStageKind(FrameStageID stage) const
	{
		return stage < GetStageCount() ? m_StageKinds[stage] : FrameStageKind::CPU;
	}

	int64_t FrameProfiler::Now() const
	{
		if (m_Specification.Clock)
			return m_Specification.Clock();
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	FrameProfiler::JobSample FrameProfiler::SampleJobs() const
	{
		const JobSystemStats stats = JobSystem::GetStats();
		JobSample sample;
		sample.Valid = true;
		sample.Time = std::chrono::duration_cast<std::chrono::nanoseconds>(stats.SampleTime.time_since_epoch()).count();
		sample.WorkerBusy = stats.Workers.BusyTime.count();
		sample.IOBusy = stats.IO.BusyTime.count();
		sample.BackgroundBusy = stats.Background.BusyTime.count();
		sample.WorkerThreads = stats.Workers.Threads;
		sample.IOThreads = stats.IO.Threads;
		sample.BackgroundThreads = stats.Background.Threads;
		return sample;
	}

	void FrameProfiler::BeginFrame()
	{
		const std::thread::id thread = std::this_thread::get_id();
		if (m_FrameThread.load(std::memory_order_relaxed) != thread)
		{
			// Stages entered on the previous frame thread end there; this thread starts without any.
			m_FrameThread.store(thread, std::memory_order_relaxed);
			m_Depth = 0;
		}

		const int64_t now = Now();
		m_Accumulated.fill(0);
		// Stages entered before the frame began count from its start.
		for (uint32_t index = 0; index < m_Depth; index++)
			m_Open[index].Start = now;
		m_FrameStart = now;
		m_InFrame.store(true, std::memory_order_relaxed);
		if (m_Specification.SampleJobs && !m_LastJobSample.Valid)
			m_LastJobSample = SampleJobs();
	}

	void FrameProfiler::EndFrame(double workSeconds)
	{
		if (!IsTimingThisThread())
			return;

		// A frame about to be overwritten is heard of by the WithGPU listeners first, GPU times or not.
		if (m_RecordedFrames == m_Capacity && m_NextGPUListenerFrame <= m_TotalFrames - m_Capacity && HasGPUListeners())
			DeliverGPUListeners(m_TotalFrames - m_Capacity + 1);

		// Stages still open count up to now here, and from now on in the next frame.
		const int64_t now = Now();
		for (uint32_t index = 0; index < m_Depth; index++)
		{
			m_Accumulated[m_Open[index].Stage] += now - m_Open[index].Start;
			m_Open[index].Start = now;
		}

		FrameRecord& record = m_Records[m_NextRecord];
		record.Index = m_TotalFrames;
		record.WorkMs = static_cast<float>(std::max(workSeconds, 0.0) * 1000.0);
		record.IntervalMs = m_HasPreviousFrame ? ToMilliseconds(m_FrameStart - m_PreviousFrameStart) : 0.0f;
		record.WorkerBusyPercent = 0.0f;
		record.IOBusyPercent = 0.0f;
		record.BackgroundBusyPercent = 0.0f;
		const uint32_t stageCount = GetStageCount();
		for (uint32_t stage = 0; stage < c_MaxStages; stage++)
			record.StageMs[stage] = stage < stageCount ? ToMilliseconds(m_Accumulated[stage]) : 0.0f;
		record.GPUResolved = false;
		record.HasGPU = false;
		record.GPUBusyMs = 0.0f;
		record.GPULatencyFrames = 0;
		record.GPUStageMs.fill(0.0f);
		if (m_Specification.SampleJobs)
		{
			const JobSample sample = SampleJobs();
			if (m_LastJobSample.Valid)
			{
				const int64_t elapsed = sample.Time - m_LastJobSample.Time;
				record.WorkerBusyPercent = ToBusyPercent(sample.WorkerBusy - m_LastJobSample.WorkerBusy, elapsed, sample.WorkerThreads);
				record.IOBusyPercent = ToBusyPercent(sample.IOBusy - m_LastJobSample.IOBusy, elapsed, sample.IOThreads);
				record.BackgroundBusyPercent = ToBusyPercent(sample.BackgroundBusy - m_LastJobSample.BackgroundBusy, elapsed, sample.BackgroundThreads);
			}
			m_LastJobSample = sample;
		}

		m_NextRecord = (m_NextRecord + 1) % m_Capacity;
		m_RecordedFrames = std::min(m_RecordedFrames + 1, m_Capacity);
		m_TotalFrames++;
		m_PreviousFrameStart = m_FrameStart;
		m_HasPreviousFrame = true;
		m_InFrame.store(false, std::memory_order_relaxed);

		if (!m_Listeners.empty())
		{
			const FrameSample sample = ToSample(record);
			for (const Listener& listener : m_Listeners)
			{
				if (listener.Timing == FrameListenerTiming::Immediate)
					listener.Callback(sample);
			}
			if (HasGPUListeners())
				DeliverGPUListeners(0);
		}
	}

	FrameProfiler::FrameRecord* FrameProfiler::FindRecord(uint64_t frameIndex)
	{
		if (frameIndex >= m_TotalFrames || m_TotalFrames - frameIndex > m_RecordedFrames)
			return nullptr;
		const uint32_t age = static_cast<uint32_t>(m_TotalFrames - 1 - frameIndex);
		FrameRecord& record = m_Records[(m_NextRecord + m_Capacity - 1 - age) % m_Capacity];
		return record.Index == frameIndex ? &record : nullptr;
	}

	bool FrameProfiler::RecordGPUFrame(uint64_t frameIndex, std::span<const GPUStageTime> stages)
	{
		FrameRecord* record = FindRecord(frameIndex);
		if (!record)
			return false;
		record->GPUStageMs.fill(0.0f);
		double busy = 0.0;
		const uint32_t stageCount = GetStageCount();
		for (const GPUStageTime& time : stages)
		{
			if (time.Stage >= stageCount || m_StageKinds[time.Stage] != FrameStageKind::GPU)
				continue;
			const float milliseconds = std::max(time.Milliseconds, 0.0f);
			record->GPUStageMs[time.Stage] += milliseconds;
			busy += milliseconds;
		}
		record->GPUResolved = true;
		record->HasGPU = !stages.empty();
		record->GPUBusyMs = static_cast<float>(busy);
		// Recorded during the frame after the latest one at the earliest: that frame counts.
		record->GPULatencyFrames = static_cast<uint32_t>(m_TotalFrames - frameIndex);
		return true;
	}

	void FrameProfiler::SetGPUTimesExpected(bool expected)
	{
		m_GPUTimesExpected = expected;
	}

	bool FrameProfiler::HasGPUListeners() const
	{
		return std::any_of(m_Listeners.begin(), m_Listeners.end(), [](const Listener& listener) { return listener.Timing == FrameListenerTiming::WithGPU; });
	}

	void FrameProfiler::DeliverGPUListeners(uint64_t forcedEnd)
	{
		// Frames that left the ring unheard (a capacity smaller than the latency) are skipped.
		const uint64_t oldest = m_TotalFrames - m_RecordedFrames;
		m_NextGPUListenerFrame = std::max(m_NextGPUListenerFrame, oldest);
		while (m_NextGPUListenerFrame < m_TotalFrames)
		{
			const FrameRecord* record = FindRecord(m_NextGPUListenerFrame);
			if (!record)
				break;
			const uint64_t laterFrames = m_TotalFrames - 1 - m_NextGPUListenerFrame;
			if (m_NextGPUListenerFrame >= forcedEnd && m_GPUTimesExpected && !record->GPUResolved && laterFrames < c_MaxGPULatencyFrames)
				break;
			const FrameSample sample = ToSample(*record);
			for (const Listener& listener : m_Listeners)
			{
				if (listener.Timing == FrameListenerTiming::WithGPU)
					listener.Callback(sample);
			}
			m_NextGPUListenerFrame++;
		}
	}

	void FrameProfiler::FlushGPUListeners()
	{
		if (HasGPUListeners())
			DeliverGPUListeners(m_TotalFrames);
	}

	bool FrameProfiler::BeginStage(FrameStageID stage)
	{
		if (!IsTimingThisThread() || stage >= GetStageCount() || m_Depth == c_MaxDepth || m_StageKinds[stage] != FrameStageKind::CPU)
			return false;
		const int64_t now = Now();
		// The stage around this one pauses: its self time excludes this one's.
		if (m_Depth > 0)
		{
			OpenStage& outer = m_Open[m_Depth - 1];
			m_Accumulated[outer.Stage] += now - outer.Start;
		}
		m_Open[m_Depth++] = OpenStage { stage, now };
		return true;
	}

	void FrameProfiler::EndStage()
	{
		if (m_Depth == 0 || !IsFrameThread())
			return;
		const int64_t now = Now();
		const OpenStage& inner = m_Open[--m_Depth];
		m_Accumulated[inner.Stage] += now - inner.Start;
		// The stage around it resumes.
		if (m_Depth > 0)
			m_Open[m_Depth - 1].Start = now;
	}

	double FrameProfiler::GetOutOfWorkSeconds() const
	{
		if (!IsTimingThisThread())
			return 0.0;
		int64_t nanoseconds = 0;
		const uint32_t stageCount = GetStageCount();
		for (uint32_t stage = 0; stage < stageCount; stage++)
		{
			if (!m_StageInWorkTime[stage])
				nanoseconds += m_Accumulated[stage];
		}
		return static_cast<double>(nanoseconds) / c_NanosecondsPerSecond;
	}

	FrameSample FrameProfiler::ToSample(const FrameRecord& record) const
	{
		FrameSample sample;
		sample.Index = record.Index;
		sample.WorkMs = record.WorkMs;
		sample.IntervalMs = record.IntervalMs;
		sample.WorkerBusyPercent = record.WorkerBusyPercent;
		sample.IOBusyPercent = record.IOBusyPercent;
		sample.BackgroundBusyPercent = record.BackgroundBusyPercent;
		sample.StageMs = std::span<const float>(record.StageMs.data(), GetStageCount());
		sample.HasGPU = record.HasGPU;
		sample.GPUBusyMs = record.GPUBusyMs;
		sample.GPUStageMs = std::span<const float>(record.GPUStageMs.data(), GetStageCount());
		sample.GPULatencyFrames = record.GPULatencyFrames;
		return sample;
	}

	FrameSample FrameProfiler::GetFrame(uint32_t age) const
	{
		ST_CORE_ASSERT(age < m_RecordedFrames, "FrameProfiler::GetFrame: frame {} of {} recorded", age, m_RecordedFrames);
		const uint32_t index = (m_NextRecord + m_Capacity - 1 - age) % m_Capacity;
		return ToSample(m_Records[index]);
	}

	FrameTimeStats FrameProfiler::ComputeStats(std::span<const float> samples)
	{
		std::vector<float> values(samples.begin(), samples.end());
		return ComputeStatsInPlace(values);
	}

	FrameTimeStats FrameProfiler::ComputeStatsInPlace(std::span<float> values)
	{
		FrameTimeStats stats;
		if (values.empty())
			return stats;
		double sum = 0.0;
		float lowest = values.front();
		float highest = values.front();
		for (const float value : values)
		{
			sum += value;
			lowest = std::min(lowest, value);
			highest = std::max(highest, value);
		}
		stats.Average = static_cast<float>(sum / static_cast<double>(values.size()));
		stats.Max = highest;
		// Samples all alike are their own percentiles: most stages in most summaries (a stage that did not run is all zeros).
		if (lowest == highest)
		{
			stats.P50 = lowest;
			stats.P95 = lowest;
			return stats;
		}
		// Nearest rank: the sample at rank ceil(p * n) (1-based), found without sorting everything. nth_element leaves the
		// samples after the median's position no smaller than the median, and the 95th percentile's rank is at least the
		// median's, so the second search looks only there.
		const auto atRank = [&values](size_t percent)
		{
			const size_t rank = std::max<size_t>((values.size() * percent + 99) / 100, 1);
			return values.begin() + static_cast<std::ptrdiff_t>(rank - 1);
		};
		const auto median = atRank(50);
		std::nth_element(values.begin(), median, values.end());
		stats.P50 = *median;
		const auto p95 = atRank(95);
		std::nth_element(median, p95, values.end());
		stats.P95 = *p95;
		return stats;
	}

	FrameProfileSummary FrameProfiler::Summarize(uint32_t frames) const
	{
		FrameProfileSummary summary;
		const uint32_t count = std::min(frames, m_RecordedFrames);
		summary.Frames = count;
		const JobSystemStats jobs = JobSystem::GetStats();
		summary.WorkerThreads = jobs.Workers.Threads;
		summary.IOThreads = jobs.IO.Threads;
		summary.BackgroundThreads = jobs.Background.Threads;

		// The stages by kind, sorted out once rather than for every frame.
		const uint32_t stageCount = GetStageCount();
		std::array<FrameStageID, c_MaxStages> cpuStages;
		std::array<FrameStageID, c_MaxStages> gpuStages;
		uint32_t cpuStageCount = 0;
		uint32_t gpuStageCount = 0;
		for (FrameStageID stage = 0; stage < stageCount; stage++)
		{
			if (m_StageKinds[stage] == FrameStageKind::GPU)
				gpuStages[gpuStageCount++] = stage;
			else
				cpuStages[cpuStageCount++] = stage;
		}

		// Every series in one allocation, a row of `count` samples each: a row per stage (a CPU stage has a sample per frame,
		// a GPU stage one per frame whose GPU times arrived), then the frames' work, stage sum, untimed time, interval (frames
		// that have a previous one) and GPU busy time. The statistics reorder each row in place: the rows are this summary's.
		const uint32_t workRow = stageCount;
		const uint32_t stageSumRow = stageCount + 1;
		const uint32_t untimedRow = stageCount + 2;
		const uint32_t intervalRow = stageCount + 3;
		const uint32_t gpuBusyRow = stageCount + 4;
		std::vector<float> samples(static_cast<size_t>(gpuBusyRow + 1) * count);
		const auto row = [&samples, count](uint32_t index) { return samples.data() + static_cast<size_t>(index) * count; };
		uint32_t gpuFrames = 0;
		uint32_t intervalCount = 0;
		double intervals = 0.0;
		double gpuLatency = 0.0;
		double workerBusy = 0.0;
		double ioBusy = 0.0;
		double backgroundBusy = 0.0;
		for (uint32_t age = 0; age < count; age++)
		{
			const FrameRecord& record = m_Records[(m_NextRecord + m_Capacity - 1 - age) % m_Capacity];
			double sum = 0.0;
			for (uint32_t index = 0; index < cpuStageCount; index++)
			{
				const FrameStageID stage = cpuStages[index];
				const float ms = record.StageMs[stage];
				row(stage)[age] = ms;
				if (m_StageInWorkTime[stage])
					sum += ms;
			}
			if (record.HasGPU)
			{
				// GPU stages over the frames whose GPU times arrived.
				for (uint32_t index = 0; index < gpuStageCount; index++)
					row(gpuStages[index])[gpuFrames] = record.GPUStageMs[gpuStages[index]];
				row(gpuBusyRow)[gpuFrames] = record.GPUBusyMs;
				gpuFrames++;
				gpuLatency += record.GPULatencyFrames;
			}
			row(workRow)[age] = record.WorkMs;
			row(stageSumRow)[age] = static_cast<float>(sum);
			row(untimedRow)[age] = std::max(record.WorkMs - static_cast<float>(sum), 0.0f);
			if (record.IntervalMs > 0.0f)
			{
				intervals += record.IntervalMs;
				row(intervalRow)[intervalCount++] = record.IntervalMs;
			}
			workerBusy += record.WorkerBusyPercent;
			ioBusy += record.IOBusyPercent;
			backgroundBusy += record.BackgroundBusyPercent;
		}

		const auto stats = [](float* first, uint32_t size) { return ComputeStatsInPlace(std::span<float>(first, size)); };
		summary.Work = stats(row(workRow), count);
		summary.StageSum = stats(row(stageSumRow), count);
		summary.Untimed = stats(row(untimedRow), count);
		summary.Interval = stats(row(intervalRow), intervalCount);
		summary.FramesPerSecond = intervals > 0.0 ? static_cast<float>(static_cast<double>(intervalCount) * 1000.0 / intervals) : 0.0f;
		if (count > 0)
		{
			summary.WorkerBusyPercent = static_cast<float>(workerBusy / static_cast<double>(count));
			summary.IOBusyPercent = static_cast<float>(ioBusy / static_cast<double>(count));
			summary.BackgroundBusyPercent = static_cast<float>(backgroundBusy / static_cast<double>(count));
		}
		summary.Stages.reserve(stageCount);
		for (FrameStageID stage = 0; stage < stageCount; stage++)
		{
			const uint32_t size = m_StageKinds[stage] == FrameStageKind::GPU ? gpuFrames : count;
			summary.Stages.push_back(FrameStageSummary { m_StageNames[stage], m_StageInWorkTime[stage], m_StageKinds[stage], stats(row(stage), size) });
		}

		FrameGPUSummary& gpu = summary.GPU;
		gpu.Frames = gpuFrames;
		gpu.Busy = stats(row(gpuBusyRow), gpuFrames);
		if (gpu.Frames > 0)
		{
			gpu.LatencyFrames = static_cast<float>(gpuLatency / static_cast<double>(gpu.Frames));
			gpu.BoundBy = ClassifyFrameBound(summary.Work.P50, gpu.Busy.P50, summary.Interval.P50);
		}
		return summary;
	}

	FrameBound FrameProfiler::ClassifyFrameBound(float workMs, float gpuBusyMs, float intervalMs)
	{
		if (intervalMs <= 0.0f)
			return FrameBound::Unknown;
		if (gpuBusyMs >= c_BoundShare * intervalMs && gpuBusyMs > workMs)
			return FrameBound::GPU;
		if (workMs >= c_BoundShare * intervalMs)
			return FrameBound::CPU;
		return FrameBound::Capped;
	}

	uint32_t FrameProfiler::AddFrameListener(std::function<void(const FrameSample&)> listener, FrameListenerTiming timing)
	{
		// A WithGPU listener hears of the frames recorded from now on.
		if (timing == FrameListenerTiming::WithGPU && !HasGPUListeners())
			m_NextGPUListenerFrame = m_TotalFrames;
		const uint32_t id = m_NextListenerID++;
		m_Listeners.push_back(Listener { id, timing, std::move(listener) });
		return id;
	}

	void FrameProfiler::RemoveFrameListener(uint32_t id)
	{
		std::erase_if(m_Listeners, [id](const Listener& listener) { return listener.ID == id; });
	}

	void FrameProfiler::Reset()
	{
		m_NextRecord = 0;
		m_RecordedFrames = 0;
		m_TotalFrames = 0;
		m_HasPreviousFrame = false;
		m_InFrame.store(false, std::memory_order_relaxed);
		m_Depth = 0;
		m_LastJobSample = {};
		m_NextGPUListenerFrame = 0;
		m_ResetCount++;
	}

}
