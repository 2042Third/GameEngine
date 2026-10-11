#include "stpch.h"
#include "Strata/Runtime/FramePerfLog.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"

#include <iterator>

namespace Strata
{

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;

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

	Scope<FramePerfLog> FramePerfLog::Create(const std::filesystem::path& path, const FrameProfiler& profiler, std::string* outError)
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
		log->m_File.open(path, std::ios::out | std::ios::trunc | std::ios::binary);
		if (!log->m_File.is_open())
			return fail(fmt::format("cannot create '{}'", FileSystem::ToUTF8(path)));

		log->m_Columns = { "Frame", "WorkMs", "IntervalMs" };
		log->m_StageCount = profiler.GetStageCount();
		for (FrameStageID stage = 0; stage < log->m_StageCount; stage++)
			log->m_Columns.push_back(profiler.GetStageName(stage));
		for (const char* column : { "WorkerBusyPercent", "IOBusyPercent", "LoadingAssets", "QueuedLoads", "InFlightLoads", "InFlightMB", "UploadedMB",
			"FinalizeMs", "ResidentGpuTexturesMB", "ResidentGpuBuffersMB", "ResidentCpuMB", "Evictions", "PrivateMB", "PeakPrivateMB" })
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

	bool FramePerfLog::WriteFrame(const FrameSample& frame, const AssetManagerStats* assets)
	{
		if (m_Failed || m_Closed)
			return false;

		m_Line.clear();
		auto out = std::back_inserter(m_Line);
		fmt::format_to(out, "{},{:.4f},{:.4f}", frame.Index, frame.WorkMs, frame.IntervalMs);
		// Stages registered after the header was written have no column.
		for (uint32_t stage = 0; stage < m_StageCount; stage++)
			fmt::format_to(out, ",{:.4f}", stage < frame.StageMs.size() ? frame.StageMs[stage] : 0.0f);
		fmt::format_to(out, ",{:.2f},{:.2f}", frame.WorkerBusyPercent, frame.IOBusyPercent);
		if (assets)
		{
			const uint32_t queued = assets->QueuedLoads[0] + assets->QueuedLoads[1] + assets->QueuedLoads[2];
			fmt::format_to(out, ",{},{},{},{:.3f},{:.3f},{:.4f},{:.3f},{:.3f},{:.3f},{}", assets->LoadingAssets, queued, assets->InFlightLoads,
				ToMB(assets->InFlightBytes), ToMB(assets->UploadedBytesLastFrame), assets->FinalizeMsLastFrame, ToMB(assets->Resident.GpuTextures),
				ToMB(assets->Resident.GpuBuffers), ToMB(assets->Resident.Cpu), assets->Evictions);
		}
		else
		{
			m_Line += ",,,,,,,,,,";
		}
		// What the process holds, for memory limits of whole games (a peak includes every frame before).
		if (const std::optional<ProcessMemoryInfo> memory = Platform::GetProcessMemory())
			fmt::format_to(out, ",{:.3f},{:.3f}", ToMB(memory->PrivateBytes), ToMB(memory->PeakPrivateBytes));
		else
			m_Line += ",,";
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
