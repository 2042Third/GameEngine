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

	FrameStageID FrameProfiler::RegisterStage(std::string_view name, bool inWorkTime)
	{
		std::scoped_lock<std::mutex> lock(m_StageMutex);
		const uint32_t count = m_StageCount.load(std::memory_order_relaxed);
		for (uint32_t index = 0; index < count; index++)
		{
			if (m_StageNames[index] == name)
				return index;
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
		m_StageInWorkTime[count] = inWorkTime;
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
		sample.WorkerThreads = stats.Workers.Threads;
		sample.IOThreads = stats.IO.Threads;
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
		const uint32_t stageCount = GetStageCount();
		for (uint32_t stage = 0; stage < c_MaxStages; stage++)
			record.StageMs[stage] = stage < stageCount ? ToMilliseconds(m_Accumulated[stage]) : 0.0f;
		if (m_Specification.SampleJobs)
		{
			const JobSample sample = SampleJobs();
			if (m_LastJobSample.Valid)
			{
				const int64_t elapsed = sample.Time - m_LastJobSample.Time;
				record.WorkerBusyPercent = ToBusyPercent(sample.WorkerBusy - m_LastJobSample.WorkerBusy, elapsed, sample.WorkerThreads);
				record.IOBusyPercent = ToBusyPercent(sample.IOBusy - m_LastJobSample.IOBusy, elapsed, sample.IOThreads);
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
			for (const auto& [id, listener] : m_Listeners)
				listener(sample);
		}
	}

	bool FrameProfiler::BeginStage(FrameStageID stage)
	{
		if (!IsTimingThisThread() || stage >= GetStageCount() || m_Depth == c_MaxDepth)
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

	FrameSample FrameProfiler::ToSample(const FrameRecord& record) const
	{
		FrameSample sample;
		sample.Index = record.Index;
		sample.WorkMs = record.WorkMs;
		sample.IntervalMs = record.IntervalMs;
		sample.WorkerBusyPercent = record.WorkerBusyPercent;
		sample.IOBusyPercent = record.IOBusyPercent;
		sample.StageMs = std::span<const float>(record.StageMs.data(), GetStageCount());
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
		FrameTimeStats stats;
		if (samples.empty())
			return stats;
		std::vector<float> values(samples.begin(), samples.end());
		double sum = 0.0;
		for (const float sample : values)
			sum += sample;
		stats.Average = static_cast<float>(sum / static_cast<double>(values.size()));
		// Nearest rank: the sample at rank ceil(p * n) (1-based), found without sorting everything.
		const auto percentile = [&values](size_t percent)
		{
			const size_t rank = std::max<size_t>((values.size() * percent + 99) / 100, 1);
			const auto it = values.begin() + static_cast<std::ptrdiff_t>(rank - 1);
			std::nth_element(values.begin(), it, values.end());
			return *it;
		};
		stats.P50 = percentile(50);
		stats.P95 = percentile(95);
		stats.Max = *std::max_element(values.begin(), values.end());
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

		const uint32_t stageCount = GetStageCount();
		std::vector<std::vector<float>> stageSamples(stageCount);
		std::vector<float> work;
		std::vector<float> stageSums;
		std::vector<float> untimed;
		work.reserve(count);
		stageSums.reserve(count);
		untimed.reserve(count);
		double intervals = 0.0;
		uint32_t intervalCount = 0;
		double workerBusy = 0.0;
		double ioBusy = 0.0;
		for (uint32_t age = 0; age < count; age++)
		{
			const FrameSample frame = GetFrame(age);
			work.push_back(frame.WorkMs);
			double sum = 0.0;
			for (uint32_t stage = 0; stage < stageCount; stage++)
			{
				stageSamples[stage].push_back(frame.StageMs[stage]);
				if (m_StageInWorkTime[stage])
					sum += frame.StageMs[stage];
			}
			stageSums.push_back(static_cast<float>(sum));
			untimed.push_back(std::max(frame.WorkMs - static_cast<float>(sum), 0.0f));
			if (frame.IntervalMs > 0.0f)
			{
				intervals += frame.IntervalMs;
				intervalCount++;
			}
			workerBusy += frame.WorkerBusyPercent;
			ioBusy += frame.IOBusyPercent;
		}

		summary.Work = ComputeStats(work);
		summary.StageSum = ComputeStats(stageSums);
		summary.Untimed = ComputeStats(untimed);
		summary.FramesPerSecond = intervals > 0.0 ? static_cast<float>(static_cast<double>(intervalCount) * 1000.0 / intervals) : 0.0f;
		if (count > 0)
		{
			summary.WorkerBusyPercent = static_cast<float>(workerBusy / static_cast<double>(count));
			summary.IOBusyPercent = static_cast<float>(ioBusy / static_cast<double>(count));
		}
		summary.Stages.reserve(stageCount);
		for (uint32_t stage = 0; stage < stageCount; stage++)
			summary.Stages.push_back(FrameStageSummary { m_StageNames[stage], m_StageInWorkTime[stage], ComputeStats(stageSamples[stage]) });
		return summary;
	}

	uint32_t FrameProfiler::AddFrameListener(std::function<void(const FrameSample&)> listener)
	{
		const uint32_t id = m_NextListenerID++;
		m_Listeners.emplace_back(id, std::move(listener));
		return id;
	}

	void FrameProfiler::RemoveFrameListener(uint32_t id)
	{
		std::erase_if(m_Listeners, [id](const auto& entry) { return entry.first == id; });
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
	}

}
