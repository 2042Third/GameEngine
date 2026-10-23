#include "stpch.h"
#include "Strata/Renderer/GPUProfiler.h"

#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/Renderer.h"

#include <algorithm>
#include <chrono>

namespace Strata
{

	namespace
	{

		// Adds the time it lives to a counter of milliseconds.
		class CostScope
		{
		public:
			explicit CostScope(double& milliseconds)
				: m_Milliseconds(milliseconds), m_Start(std::chrono::steady_clock::now())
			{
			}

			~CostScope()
			{
				m_Milliseconds += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_Start).count();
			}

			CostScope(const CostScope&) = delete;
			CostScope& operator=(const CostScope&) = delete;
		private:
			double& m_Milliseconds;
			std::chrono::steady_clock::time_point m_Start;
		};

		FrameStageID RegisterGPUStage(std::string_view name)
		{
			return FrameProfiler::Get().RegisterStage(name, false, FrameStageKind::GPU);
		}

	}

	const RendererGPUStages& RendererGPUStages::Get()
	{
		// Registered in this order, so the stages have the same ids (and perf log columns) in every program.
		static const RendererGPUStages s_Stages = []()
		{
			RendererGPUStages stages;
			stages.Uploads = RegisterGPUStage("GPU.Uploads");
			stages.Setup = RegisterGPUStage("GPU.Setup");
			stages.Cull = RegisterGPUStage("GPU.Cull");
			stages.Shadows = RegisterGPUStage("GPU.Shadows");
			stages.Prepass = RegisterGPUStage("GPU.Prepass");
			stages.AO = RegisterGPUStage("GPU.AO");
			stages.Reflections = RegisterGPUStage("GPU.Reflections");
			stages.Opaque = RegisterGPUStage("GPU.Opaque");
			stages.Transparent = RegisterGPUStage("GPU.Transparent");
			stages.TAA = RegisterGPUStage("GPU.TAA");
			stages.Post = RegisterGPUStage("GPU.Post");
			stages.Overlays = RegisterGPUStage("GPU.Overlays");
			return stages;
		}();
		return s_Stages;
	}

	GPUProfiler::GPUProfiler(GraphicsDevice& device, FrameProfiler& profiler)
		: m_Device(device), m_Profiler(profiler)
	{
		const GraphicsDeviceInfo& info = device.GetInfo();
		const uint32_t slots = device.GetMaxFramesInFlight() + 2;
		m_Slots.resize(slots);
		if (!info.SupportsTimestamps)
			return;
		m_Pool = device.CreateTimestampPool(slots * c_MaxTimestampsPerFrame);
		if (!m_Pool)
			return;
		m_PeriodNs = info.TimestampPeriodNs;
		m_Ticks.resize(c_MaxTimestampsPerFrame);
		for (Slot& slot : m_Slots)
			slot.Intervals.reserve(64);
		m_StageTimes.reserve(FrameProfiler::c_MaxStages);
		m_Profiler.SetGPUTimesExpected(true);
	}

	GPUProfiler::~GPUProfiler()
	{
		if (m_Pool)
			m_Profiler.SetGPUTimesExpected(false);
	}

	void GPUProfiler::BeginFrame()
	{
		if (!m_Pool)
			return;
		CostScope cost(m_Stats.CPUMilliseconds);
		CloseOpenSlot();
		ResolvePending();
		OpenSlot();
	}

	void GPUProfiler::Resolve(bool wait)
	{
		if (!m_Pool)
			return;
		if (wait)
		{
			m_Device.WaitForIdle();
			// Everything the open slot holds so far ran (its reset too): its users may read those (ReadRecord).
			if (m_OpenSlot < m_Slots.size())
				m_Slots[m_OpenSlot].CompletedCount = m_Slots[m_OpenSlot].Count;
		}
		CostScope cost(m_Stats.CPUMilliseconds);
		ResolvePending();
	}

	void GPUProfiler::CloseOpenSlot()
	{
		if (m_OpenSlot >= m_Slots.size())
			return;
		Slot& slot = m_Slots[m_OpenSlot];
		m_OpenSlot = ~0u;
		if (slot.Count == 0)
		{
			// Nothing timed: the frame had no GPU work to report.
			if (slot.ResetCount == m_Profiler.GetResetCount())
				m_Profiler.RecordGPUFrame(slot.ProfilerFrame, {});
			FreeSlot(slot);
			return;
		}
		// Done once the GPU completed every command list submitted so far: the frame's work.
		if (!slot.Done)
			slot.Done = m_Device.GetDevice()->createEventQuery();
		if (slot.Done)
		{
			m_Device.GetDevice()->resetEventQuery(slot.Done);
			m_Device.GetDevice()->setEventQuery(slot.Done, nvrhi::CommandQueue::Graphics);
			slot.DoneSet = true;
		}
		slot.State = SlotState::Pending;
	}

	void GPUProfiler::OpenSlot()
	{
		Slot& slot = m_Slots[m_NextSlot];
		if (slot.State == SlotState::Pending)
		{
			// Its turn came before its times: dropped. Its timestamps may be reset only once the GPU is done with them; until
			// then this frame goes untimed.
			if (!IsSlotDone(slot))
			{
				m_Stats.DroppedFrames++;
				return;
			}
			m_Stats.DroppedFrames++;
			FreeSlot(slot);
		}
		m_Pool->Reset(GetBase(m_NextSlot), c_MaxTimestampsPerFrame);
		slot.State = SlotState::Open;
		slot.Serial = m_NextSerial++;
		slot.ProfilerFrame = m_Profiler.GetTotalFrameCount();
		slot.ResetCount = m_Profiler.GetResetCount();
		slot.Count = 0;
		slot.CompletedCount = 0;
		slot.Intervals.clear();
		m_OpenSlot = m_NextSlot;
		m_NextSlot = (m_NextSlot + 1) % static_cast<uint32_t>(m_Slots.size());
	}

	bool GPUProfiler::IsSlotDone(Slot& slot)
	{
		return !slot.DoneSet || m_Device.GetDevice()->pollEventQuery(slot.Done);
	}

	void GPUProfiler::FreeSlot(Slot& slot)
	{
		slot.State = SlotState::Free;
		slot.Count = 0;
		slot.Unreadable = false;
		slot.Intervals.clear();
	}

	void GPUProfiler::ResolvePending()
	{
		// Oldest first: the GPU completes the frames in order, so a slot it has not completed ends the search.
		while (true)
		{
			uint32_t oldest = ~0u;
			for (uint32_t index = 0; index < m_Slots.size(); index++)
			{
				const Slot& slot = m_Slots[index];
				if (slot.State == SlotState::Pending && (oldest == ~0u || slot.Serial < m_Slots[oldest].Serial))
					oldest = index;
			}
			if (oldest == ~0u)
				return;
			Slot& slot = m_Slots[oldest];
			// Read once the GPU completed the frame: before that, its reset may not have run yet either.
			if (!IsSlotDone(slot))
				return;
			if (ResolveSlot(slot, oldest))
				continue;
			// The GPU completed the frame's work, yet not every timestamp is readable. MoltenVK makes timestamps readable a
			// little after the work's completion is signaled: the slot waits for the next frame. Still missing then, a command
			// list with timestamps was never executed: its times are incomplete, so the frame goes without them.
			const uint64_t frame = m_Profiler.GetTotalFrameCount();
			if (!slot.Unreadable)
			{
				slot.Unreadable = true;
				slot.UnreadableFrame = frame;
				return;
			}
			if (frame == slot.UnreadableFrame)
				return;
			m_Stats.DroppedFrames++;
			if (slot.ResetCount == m_Profiler.GetResetCount())
				m_Profiler.RecordGPUFrame(slot.ProfilerFrame, {});
			FreeSlot(slot);
		}
	}

	bool GPUProfiler::ResolveSlot(Slot& slot, uint32_t index)
	{
		if (!m_Pool->Read(GetBase(index), slot.Count, m_Ticks))
			return false;
		const uint32_t stageCount = m_Profiler.GetStageCount();
		const uint32_t base = GetBase(index);
		std::fill(m_StageSums.begin(), m_StageSums.end(), 0.0f);
		std::array<bool, FrameProfiler::c_MaxStages> timed = {};
		for (const GPUTimedInterval& interval : slot.Intervals)
		{
			if (interval.Stage >= stageCount)
				continue;
			m_StageSums[interval.Stage] += ToMilliseconds(m_Ticks[interval.Begin - base], m_Ticks[interval.End - base]);
			timed[interval.Stage] = true;
		}
		// Every stage the frame timed, also one that took no measurable time.
		m_StageTimes.clear();
		for (uint32_t stage = 0; stage < stageCount; stage++)
		{
			if (timed[stage])
				m_StageTimes.push_back(GPUStageTime { stage, m_StageSums[stage] });
		}
		if (slot.ResetCount == m_Profiler.GetResetCount())
			m_Profiler.RecordGPUFrame(slot.ProfilerFrame, m_StageTimes);
		m_Stats.ResolvedFrames++;
		FreeSlot(slot);
		return true;
	}

	float GPUProfiler::ToMilliseconds(uint64_t begin, uint64_t end) const
	{
		const uint64_t ticks = (end - begin) & m_Pool->GetValueMask();
		return static_cast<float>(static_cast<double>(ticks) * m_PeriodNs * 1.0e-6);
	}

	uint64_t GPUProfiler::GetOpenSlot() const
	{
		return m_OpenSlot < m_Slots.size() ? m_Slots[m_OpenSlot].Serial : 0;
	}

	uint32_t GPUProfiler::WriteTimestamp(nvrhi::ICommandList* commandList)
	{
		if (!m_Pool || m_OpenSlot >= m_Slots.size() || !commandList)
			return c_NoTimestamp;
		CostScope cost(m_Stats.CPUMilliseconds);
		Slot& slot = m_Slots[m_OpenSlot];
		if (slot.Count == c_MaxTimestampsPerFrame)
		{
			m_Stats.SkippedTimestamps++;
			return c_NoTimestamp;
		}
		const uint32_t index = GetBase(m_OpenSlot) + slot.Count++;
		m_Pool->Write(commandList, index);
		m_Stats.Timestamps++;
		return index;
	}

	void GPUProfiler::AddInterval(FrameStageID stage, uint32_t begin, uint32_t end, GPUTimerRecord* record)
	{
		if (m_OpenSlot >= m_Slots.size() || begin == c_NoTimestamp || end == c_NoTimestamp || stage == c_InvalidFrameStage)
			return;
		Slot& slot = m_Slots[m_OpenSlot];
		const GPUTimedInterval interval { stage, begin, end };
		slot.Intervals.push_back(interval);
		if (record)
		{
			if (record->Slot != slot.Serial)
			{
				record->Clear();
				record->Slot = slot.Serial;
				record->ProfilerFrame = slot.ProfilerFrame;
			}
			record->Intervals.push_back(interval);
		}
	}

	bool GPUProfiler::ReadRecord(const GPUTimerRecord& record, std::vector<GPUStageTime>& outStages)
	{
		outStages.clear();
		if (!m_Pool || record.Slot == 0)
			return false;
		// A slot keeps its serial until it is opened again (and reset): a resolved (freed) slot still holds its times.
		const auto slot = std::find_if(m_Slots.begin(), m_Slots.end(), [&record](const Slot& candidate) { return candidate.Serial == record.Slot; });
		if (slot == m_Slots.end())
			return false;
		// Its timestamps hold this frame's values only once the GPU ran the slot's reset: for a closed slot once its frame
		// completed, for the open one what a Resolve(true) saw written.
		if (slot->State == SlotState::Pending && !IsSlotDone(*slot))
			return false;
		const uint32_t base = GetBase(static_cast<uint32_t>(slot - m_Slots.begin()));
		std::array<uint64_t, 2> ticks = {};
		for (const GPUTimedInterval& interval : record.Intervals)
		{
			if (slot->State == SlotState::Open && interval.End >= base + slot->CompletedCount)
			{
				outStages.clear();
				return false;
			}
			if (!m_Pool->Read(interval.Begin, 1, std::span<uint64_t>(ticks.data(), 1)) || !m_Pool->Read(interval.End, 1, std::span<uint64_t>(ticks.data() + 1, 1)))
			{
				outStages.clear();
				return false;
			}
			const float milliseconds = ToMilliseconds(ticks[0], ticks[1]);
			const auto existing = std::find_if(outStages.begin(), outStages.end(), [&interval](const GPUStageTime& time) { return time.Stage == interval.Stage; });
			if (existing != outStages.end())
				existing->Milliseconds += milliseconds;
			else
				outStages.push_back(GPUStageTime { interval.Stage, milliseconds });
		}
		return true;
	}

	GPUPassTimer::GPUPassTimer(nvrhi::ICommandList* commandList, FrameStageID stage, GPUTimerRecord* record)
		: GPUPassTimer(Renderer::FindGPUProfiler(), commandList, stage, record)
	{
	}

	GPUPassTimer::GPUPassTimer(GPUProfiler* profiler, nvrhi::ICommandList* commandList, FrameStageID stage, GPUTimerRecord* record)
		: m_Profiler(profiler && profiler->IsEnabled() ? profiler : nullptr), m_CommandList(commandList), m_Record(record), m_Stage(stage)
	{
		if (!m_Profiler)
			return;
		m_Slot = m_Profiler->GetOpenSlot();
		m_Begin = m_Profiler->WriteTimestamp(m_CommandList);
	}

	void GPUPassTimer::Next(FrameStageID stage)
	{
		if (!m_Profiler)
		{
			m_Stage = stage;
			return;
		}
		if (stage == m_Stage)
			return;
		const uint32_t boundary = m_Profiler->WriteTimestamp(m_CommandList);
		// A frame boundary between the two timestamps (the slot changed) leaves the interval out.
		if (m_Profiler->GetOpenSlot() == m_Slot)
			m_Profiler->AddInterval(m_Stage, m_Begin, boundary, m_Record);
		m_Slot = m_Profiler->GetOpenSlot();
		m_Begin = boundary;
		m_Stage = stage;
	}

	void GPUPassTimer::End()
	{
		if (!m_Profiler)
			return;
		const uint32_t end = m_Profiler->WriteTimestamp(m_CommandList);
		if (m_Profiler->GetOpenSlot() == m_Slot)
			m_Profiler->AddInterval(m_Stage, m_Begin, end, m_Record);
		m_Profiler = nullptr;
	}

}
