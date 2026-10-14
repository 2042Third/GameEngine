#include "stpch.h"
#include "Strata/Runtime/FramePerfLog.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Renderer/GPUUploadQueue.h"
#include "Strata/Scene/SceneSystem.h"

#include <iterator>

namespace Strata
{

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;
		// The columns FramePerfLog::CaptureFrame fills: the asset manager's, the upload queue's, the device's and the process's.
		constexpr size_t c_CapturedColumns = 15 + 3 + 2 + 2;

		// A header field as CSV (RFC 4180): quoted when it holds a separator, a quote or a line break.
		std::string QuoteField(const std::string& field)
		{
			if (field.find_first_of(",\"\r\n") == std::string::npos)
				return field;
			std::string quoted = "\"";
			for (const char character : field)
			{
				if (character == '"')
					quoted += '"';
				quoted += character;
			}
			quoted += '"';
			return quoted;
		}

		double ToMB(uint64_t bytes)
		{
			return static_cast<double>(bytes) / c_BytesPerMB;
		}

	}

	Scope<FramePerfLog> FramePerfLog::Create(const std::filesystem::path& path, FrameProfiler& profiler, std::string* outError)
	{
		const auto fail = [&](std::string message) -> Scope<FramePerfLog>
		{
			if (outError)
				*outError = std::move(message);
			return nullptr;
		};

		if (path.has_parent_path() && !FileSystem::CreateDirectories(path.parent_path()))
			return fail(fmt::format("cannot create the folder of '{}'", FileSystem::ToUTF8(path)));
		Scope<FramePerfLog> log(new FramePerfLog());
		log->m_Path = path;
		log->m_Profiler = &profiler;
		log->m_File.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
		if (!log->m_File.is_open())
			return fail(fmt::format("cannot create '{}'", FileSystem::ToUTF8(path)));

		// Scene systems register their stages when a scene first runs them: registered now, every system has a column.
		for (const SceneSystemDescriptor& system : SceneSystemRegistry::GetAll())
		{
			if (!system.ProfileStage.empty())
				profiler.RegisterStage(system.ProfileStage);
		}

		log->m_Columns = { "Frame", "WorkMs", "IntervalMs" };
		log->m_StageCount = profiler.GetStageCount();
		log->m_WarnedStages = log->m_StageCount;
		for (FrameStageID stage = 0; stage < log->m_StageCount; stage++)
		{
			log->m_Columns.push_back(profiler.GetStageName(stage));
			log->m_GPUStages.push_back(profiler.GetStageKind(stage) == FrameStageKind::GPU);
		}
		for (const char* column : { "WorkerBusyPercent", "IOBusyPercent", "LoadingAssets", "QueuedLoads", "InFlightLoads", "InFlightMB", "UploadedMB",
			"FinalizeMs", "UploadMs", "ResidentGpuTexturesMB", "ResidentGpuBuffersMB", "ResidentCpuMB", "Evictions", "StreamedMB", "MipLevelsRaised",
			"MipLevelsDropped", "MipsBelowWanted", "UploadQueueMB", "UploadQueueMs", "UploadQueueSteps", "DeviceMB", "StagingMB", "PrivateMB",
			"PeakPrivateMB", "GPUFrame", "GPUBusyMs" })
		{
			log->m_Columns.push_back(column);
		}

		std::string header;
		for (size_t index = 0; index < log->m_Columns.size(); index++)
		{
			if (index > 0)
				header += ',';
			header += QuoteField(log->m_Columns[index]);
		}
		header += '\n';
		log->m_File.write(header.data(), static_cast<std::streamsize>(header.size()));
		if (!log->m_File.good())
			return fail(fmt::format("cannot write to '{}'", FileSystem::ToUTF8(path)));
		return log;
	}

	FramePerfLog::~FramePerfLog()
	{
		Close();
	}

	void FramePerfLog::FormatCapture(std::string& line, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads, const FramePerfDeviceMemory* device)
	{
		auto out = std::back_inserter(line);
		if (assets)
		{
			const uint32_t queued = assets->QueuedLoads[0] + assets->QueuedLoads[1] + assets->QueuedLoads[2];
			fmt::format_to(out, ",{},{},{},{:.3f},{:.3f},{:.4f},{:.4f},{:.3f},{:.3f},{:.3f},{}", assets->LoadingAssets, queued, assets->InFlightLoads,
				ToMB(assets->InFlightBytes), ToMB(assets->UploadedBytesLastFrame), assets->FinalizeMsLastFrame, assets->UploadMsLastFrame, ToMB(assets->Resident.GpuTextures),
				ToMB(assets->Resident.GpuBuffers), ToMB(assets->Resident.Cpu), assets->Evictions);
			fmt::format_to(out, ",{:.3f},{},{},{}", ToMB(assets->Detail.BytesStreamedLastFrame), assets->Detail.LevelsRaised, assets->Detail.LevelsDropped,
				assets->Detail.AssetsBelowWanted);
		}
		else
		{
			line += ",,,,,,,,,,,,,,,";
		}
		// The frame's steps through the upload queue (it is captured before the next frame begins).
		if (uploads)
			fmt::format_to(out, ",{:.3f},{:.4f},{}", ToMB(uploads->FrameBytes), uploads->FrameMs, uploads->FrameSteps);
		else
			line += ",,,";
		if (device)
			fmt::format_to(out, ",{:.3f},{:.3f}", ToMB(device->DeviceBytes), ToMB(device->StagingBytes));
		else
			line += ",,";
		// What the process holds, for memory limits of whole games (a peak includes every frame before).
		if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
			fmt::format_to(out, ",{:.3f},{:.3f}", ToMB(memory->PrivateBytes), ToMB(memory->PeakPrivateBytes));
		else
			line += ",,";
	}

	void FramePerfLog::CaptureFrame(const FrameSample& frame, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads, const FramePerfDeviceMemory* device)
	{
		if (m_Failed || m_Closed)
			return;
		CapturedFrame captured;
		captured.Index = frame.Index;
		FormatCapture(captured.Columns, assets, uploads, device);
		m_Captured.push_back(std::move(captured));
	}

	bool FramePerfLog::WriteFrame(const FrameSample& frame, const AssetManagerStats* assets, const GPUUploadQueueStats* uploads, const FramePerfDeviceMemory* device)
	{
		CaptureFrame(frame, assets, uploads, device);
		return WriteFrame(frame);
	}

	bool FramePerfLog::WriteFrame(const FrameSample& frame)
	{
		if (m_Failed || m_Closed)
			return false;

		// Stages registered after the header was written have no column: said once per stage.
		for (uint32_t stage = m_WarnedStages; stage < frame.StageMs.size(); stage++)
		{
			ST_CORE_WARN("The perf log '{}' has no column for stage '{}', registered after the log was created", FileSystem::ToUTF8(m_Path),
				m_Profiler->GetStageName(stage));
		}
		m_WarnedStages = std::max(m_WarnedStages, static_cast<uint32_t>(frame.StageMs.size()));

		// The frame's captured columns; frames captured before it that never got a row (a listener added later) go.
		while (!m_Captured.empty() && m_Captured.front().Index < frame.Index)
			m_Captured.pop_front();

		m_Line.clear();
		auto out = std::back_inserter(m_Line);
		fmt::format_to(out, "{},{:.4f},{:.4f}", frame.Index, frame.WorkMs, frame.IntervalMs);
		for (uint32_t stage = 0; stage < m_StageCount; stage++)
		{
			if (!m_GPUStages[stage])
				fmt::format_to(out, ",{:.4f}", stage < frame.StageMs.size() ? frame.StageMs[stage] : 0.0f);
			else if (frame.HasGPU && stage < frame.GPUStageMs.size())
				fmt::format_to(out, ",{:.4f}", frame.GPUStageMs[stage]);
			else
				m_Line += ',';
		}
		fmt::format_to(out, ",{:.2f},{:.2f}", frame.WorkerBusyPercent, frame.IOBusyPercent);
		if (!m_Captured.empty() && m_Captured.front().Index == frame.Index)
		{
			m_Line += m_Captured.front().Columns;
			m_Captured.pop_front();
		}
		else
		{
			m_Line.append(c_CapturedColumns, ',');
		}
		if (frame.HasGPU)
			fmt::format_to(out, ",1,{:.4f}", frame.GPUBusyMs);
		else
			m_Line += ",0,";
		m_Line += '\n';

		m_File.write(m_Line.data(), static_cast<std::streamsize>(m_Line.size()));
		if (!m_File.good())
		{
			m_Failed = true;
			ST_CORE_ERROR("Writing the perf log '{}' failed after {} rows", FileSystem::ToUTF8(m_Path), m_Rows);
			return false;
		}
		m_Rows++;
		return true;
	}

	bool FramePerfLog::Close(std::string* outError)
	{
		if (!m_Closed)
		{
			m_Closed = true;
			m_File.flush();
			if (!m_File.good())
				m_Failed = true;
			m_File.close();
		}
		if (m_Failed && outError)
			*outError = fmt::format("writing '{}' failed", FileSystem::ToUTF8(m_Path));
		return !m_Failed;
	}

}
