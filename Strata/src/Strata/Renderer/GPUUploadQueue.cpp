#include "stpch.h"
#include "Strata/Renderer/GPUUploadQueue.h"

#include "Strata/Core/JobSystem.h"
#include "Strata/Renderer/StagingPool.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace Strata
{

	namespace
	{

		// The speeds and the staging creation time steps are planned with before any was measured: writing write-combined
		// staging memory the pool had, and staging created for the step (GPUUploadQueueStats::BytesPerMs).
		constexpr float c_InitialBytesPerMs = 4.0e6f;
		constexpr float c_InitialColdBytesPerMs = 2.0e6f;
		constexpr float c_InitialStagingCreateMs = 0.5f;
		// Steps are planned to end within this share of the frame's upload time (and of the time until the deadline): a step's
		// speed strays from the measured average by a tenth and more (the memory bandwidth the frame's other work takes, page
		// faults), so steps planned to fill the budget to its end overshot it as often as not (frames that filled their 2 ms
		// took up to 2.18 ms). The reserve is a share of the frame's time, not of what is left: the steps of a frame each
		// planned within a share of what was left still added up to the budget's end (1.99 to 2.07 ms).
		constexpr double c_PlannedTimeShare = 0.9;
		// A step that writes at least this much into staging the pool had already spreads its writes over the worker threads,
		// in pieces of about c_CopyPieceBytes: one thread reads the decoded data and writes write-combined memory far slower
		// than the memory system takes them (4 MiB took 0.4 to 1.2 ms on one thread of the reference machine, 0.15 to 0.3 ms
		// spread), and the main thread waits for the step either way. Staging just created is written by the main thread
		// alone: its first writes fault its pages in, which the system serializes (spread, 4 MiB took 2.5 to 4.3 ms).
		constexpr size_t c_ParallelCopyBytes = 512 << 10;
		constexpr size_t c_CopyPieceBytes = 256 << 10;

		// Regions start at multiples of this in staging buffers: copies from buffers into textures start at multiples of the
		// texel block size, which divides it for every format.
		constexpr uint64_t c_RegionAlignment = 16;

		uint32_t AlignUp(uint32_t value, uint32_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		uint64_t AlignUp64(uint64_t value, uint64_t alignment)
		{
			return (value + alignment - 1) / alignment * alignment;
		}

		float MillisecondsSince(std::chrono::steady_clock::time_point start)
		{
			return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
		}

		nvrhi::CommandQueue ToNvrhiQueue(GPUUploadQueueKind queue)
		{
			return queue == GPUUploadQueueKind::Transfer ? nvrhi::CommandQueue::Copy : nvrhi::CommandQueue::Graphics;
		}

	}

	GPUUploadQueue::GPUUploadQueue(GraphicsDevice& device, StagingPool& staging)
		: m_GraphicsDevice(device), m_Device(device.GetDevice()), m_Staging(staging)
	{
		m_Stats.BytesPerMs = c_InitialBytesPerMs;
		m_Stats.ColdBytesPerMs = c_InitialColdBytesPerMs;
		m_Stats.StagingCreateMs = c_InitialStagingCreateMs;
	}

	GPUUploadQueue::~GPUUploadQueue()
	{
		ST_CORE_ASSERT(!m_BatchOpen, "An upload batch was opened and never submitted");
	}

	bool GPUUploadQueue::SetTransferQueue(bool enabled)
	{
		ST_CORE_ASSERT(!m_BatchOpen, "The upload queue's transfer queue is switched between batches");
		m_UseTransferQueue = enabled && m_GraphicsDevice.HasTransferQueue();
		m_Stats.TransferQueue = m_UseTransferQueue;
		return m_UseTransferQueue;
	}

	GPUUploadQueueKind GPUUploadQueue::ChooseQueue(const AssetFinalizeContext& context) const
	{
		return m_UseTransferQueue && context.MayWait ? GPUUploadQueueKind::Transfer : GPUUploadQueueKind::Graphics;
	}

	void GPUUploadQueue::BeginBatch()
	{
		ST_CORE_ASSERT(!m_BatchOpen, "Upload batches do not nest");
		// The command lists open when something is recorded (GetList): a frame without uploads submits nothing.
		m_BatchOpen = true;
		m_GraphicsOpen = false;
		m_TransferOpen = false;
	}

	nvrhi::ICommandList* GPUUploadQueue::GetList(GPUUploadQueueKind queue)
	{
		if (!m_GraphicsOpen)
		{
			if (!m_GraphicsList)
				m_GraphicsList = m_Device->createCommandList();
			m_GraphicsList->open();
			m_GraphicsOpen = true;
			m_GraphicsTimer.emplace(m_GraphicsList.Get(), RendererGPUStages::Get().Uploads);
		}
		if (queue == GPUUploadQueueKind::Graphics)
			return m_GraphicsList;
		if (!m_TransferOpen)
		{
			// Not an immediate command list: only one of those may be open at a time, and the batch's graphics list is one.
			if (!m_TransferList)
			{
				m_TransferList = m_Device->createCommandList(nvrhi::CommandListParameters().setQueueType(nvrhi::CommandQueue::Copy)
					.setEnableImmediateExecution(false));
			}
			m_TransferList->open();
			m_TransferOpen = true;
		}
		return m_TransferList;
	}

	nvrhi::EventQueryHandle GPUUploadQueue::SetQuery(nvrhi::CommandQueue queue)
	{
		nvrhi::EventQueryHandle query;
		if (!m_IdleQueries.empty())
		{
			query = std::move(m_IdleQueries.back());
			m_IdleQueries.pop_back();
		}
		else
		{
			query = m_Device->createEventQuery();
		}
		if (query)
		{
			m_Device->resetEventQuery(query);
			m_Device->setEventQuery(query, queue);
		}
		return query;
	}

	void GPUUploadQueue::SubmitBatch()
	{
		ST_CORE_ASSERT(m_BatchOpen, "An upload batch was submitted that was never opened");
		if (!m_BatchOpen)
			return;
		m_BatchOpen = false;
		if (!m_GraphicsOpen)
			return; // Nothing was recorded (the transfer list opens the graphics list too): no ticket, no submission
		m_GraphicsOpen = false;

		// The transfer queue's copies first: the graphics queue's work of the batch does not depend on them (what they wrote is
		// handed over by later batches, once they completed).
		nvrhi::EventQueryHandle transferQuery;
		bool queriesCreated = true;
		if (m_TransferOpen)
		{
			// NVRHI would return what the list wrote to the resources' initial states (shader resources, vertex buffers) when
			// it closes, which a transfer queue cannot do: their states are given back to its tracking instead (the levels the
			// list left as copy destinations go to the graphics queue by a queue family ownership transfer).
			for (const nvrhi::TextureHandle& texture : m_TransferTextures)
				m_TransferList->beginTrackingTextureState(texture, nvrhi::AllSubresources, texture->getDesc().initialState);
			for (const nvrhi::BufferHandle& buffer : m_TransferBuffers)
				m_TransferList->beginTrackingBufferState(buffer, buffer->getDesc().initialState);
			m_TransferList->close();
			m_Device->executeCommandList(m_TransferList, nvrhi::CommandQueue::Copy);
			m_Staging.FenceReleased(nvrhi::CommandQueue::Copy);
			m_TransferTextures.clear();
			m_TransferBuffers.clear();
			m_TransferOpen = false;
			transferQuery = SetQuery(nvrhi::CommandQueue::Copy);
			queriesCreated = transferQuery != nullptr;
		}

		// Executed also when only the transfer list recorded: the batch's completion is tracked on both queues.
		if (m_GraphicsTimer)
		{
			m_GraphicsTimer->End();
			m_GraphicsTimer.reset();
		}
		m_GraphicsList->close();
		m_Device->executeCommandList(m_GraphicsList);
		// The staging these uploads copy from becomes reusable as soon as the GPU has run them, usually by the next frame's
		// finalization: the GPU runs them ahead of the frame's rendering.
		m_Staging.FenceReleased(nvrhi::CommandQueue::Graphics);
		m_Stats.Batches++;

		nvrhi::EventQueryHandle graphicsQuery = SetQuery(nvrhi::CommandQueue::Graphics);
		if (graphicsQuery && queriesCreated)
		{
			m_Submissions.push_back(Submission { m_BatchTicket, std::move(graphicsQuery), std::move(transferQuery), std::move(m_BatchTextures) });
		}
		else
		{
			// Completion cannot be observed without a query: wait for the GPU instead.
			ST_CORE_ERROR("GPUUploadQueue: failed to create an event query; waiting for the GPU to complete the uploads");
			m_Device->waitForIdle();
			CollectCompleted();
			m_CompletedTicket = m_BatchTicket;
		}
		m_BatchTextures.clear();
		m_BatchTicket++;
	}

	void GPUUploadQueue::BeginFrame()
	{
		m_Stats.LastFrameBytes = m_Stats.FrameBytes;
		m_Stats.LastFrameMs = m_Stats.FrameMs;
		m_Stats.LastFrameSceneDataBytes = m_Stats.SceneDataBytes;
		m_Stats.LastFrameSceneDataMs = m_Stats.SceneDataMs;
		m_Stats.FrameBytes = 0;
		m_Stats.FrameMs = 0.0f;
		m_Stats.FrameSteps = 0;
		m_Stats.SceneDataBytes = 0;
		m_Stats.SceneDataMs = 0.0f;
		CollectCompleted();
	}

	bool GPUUploadQueue::IsFramesFirstStep(const AssetFinalizeContext& context, uint64_t callBytes)
	{
		return context.FirstOfFrame && callBytes == 0 && (!context.UploadedBytes || *context.UploadedBytes == 0);
	}

	uint64_t GPUUploadQueue::PlanStep(const AssetFinalizeContext& context, uint64_t callBytes, float callMs, uint64_t wantedBytes, uint64_t granuleBytes) const
	{
		if (wantedBytes == 0)
			return 0;
		const uint64_t granule = std::clamp<uint64_t>(granuleBytes, 1, wantedBytes);
		uint64_t limit = std::min(wantedBytes, c_MaxStepBytes);
		limit = std::min(limit, context.UploadBudget > callBytes ? context.UploadBudget - callBytes : 0);

		// What is left of the call's upload time less the frame's reserve, and of the time until the deadline less its share, at
		// the speed of the latest steps.
		double milliseconds = std::numeric_limits<double>::infinity();
		if (std::isfinite(context.UploadMsBudget))
		{
			const float frameBudget = std::isfinite(context.UploadMsFrameBudget) ? context.UploadMsFrameBudget : context.UploadMsBudget;
			const double reserve = (1.0 - c_PlannedTimeShare) * static_cast<double>(std::max(frameBudget, 0.0f));
			milliseconds = static_cast<double>(context.UploadMsBudget) - static_cast<double>(callMs) - reserve;
		}
		if (context.Deadline != std::chrono::steady_clock::time_point::max())
		{
			const auto now = std::chrono::steady_clock::now();
			const double untilDeadline = context.Deadline > now ? std::chrono::duration<double, std::milli>(context.Deadline - now).count() : 0.0;
			milliseconds = std::min(milliseconds, untilDeadline * c_PlannedTimeShare);
		}
		const bool framesFirst = IsFramesFirstStep(context, callBytes);
		const auto finish = [&](uint64_t bytes)
		{
			if (bytes >= wantedBytes)
				return wantedBytes; // All of it fits
			bytes -= bytes % granule;
			if (bytes == 0 && framesFirst)
				bytes = granule; // Every frame makes progress
			return bytes;
		};
		if (!std::isfinite(milliseconds))
			return finish(limit);

		const auto byTime = [&](double available, float bytesPerMs)
		{
			const double bytes = std::max(0.0, available) * static_cast<double>(bytesPerMs);
			return bytes < static_cast<double>(limit) ? static_cast<uint64_t>(bytes) : limit;
		};
		const uint64_t step = finish(byTime(milliseconds, m_Stats.BytesPerMs));
		// The staging the step would take (callers take what is wanted when it all fits, else a power of two of bytes): where
		// the pool has none idle, the step creates it first and then writes fresh memory. The creation takes at most half the
		// time left, so such steps still make progress (and staging of their size comes about) where creating staging is slow.
		if (step == 0 || m_Staging.HasIdleBuffer(step >= wantedBytes ? wantedBytes : std::bit_floor(step)))
			return step;
		const double creationMs = std::min(static_cast<double>(m_Stats.StagingCreateMs), milliseconds * 0.5);
		return finish(byTime(milliseconds - creationMs, m_Stats.ColdBytesPerMs));
	}

	bool GPUUploadQueue::HasStagingRoom(const AssetFinalizeContext& context, uint64_t bytes, bool framesFirstStep) const
	{
		// The frame's later steps leave a step of the budget to the next frame's first, which may use all of it (a first step
		// larger than the whole budget goes once nothing else is in flight).
		if (!framesFirstStep)
			return m_Staging.HasRoomFor(bytes, context.GetStagingRoom());
		return m_Staging.HasRoomFor(std::min(bytes, context.StagingBytes), context.StagingBytes);
	}

	uint64_t GPUUploadQueue::GetRegionBytes(TextureFormat format, const GPUTextureRegion& region)
	{
		const TextureFormatInfo info = GetTextureFormatInfo(format);
		return static_cast<uint64_t>(AlignUp(region.Rows, info.BlockHeight) / info.BlockHeight) * GetBlockRowByteSize(format, region.Width);
	}

	uint64_t GPUUploadQueue::GetStagingBytes(TextureFormat format, std::span<const GPUTextureRegion> regions)
	{
		uint64_t bytes = 0;
		for (const GPUTextureRegion& region : regions)
			bytes = AlignUp64(bytes, c_RegionAlignment) + GetRegionBytes(format, region);
		return StagingPool::GetBufferByteSize(bytes);
	}

	GPUUploadStep GPUUploadQueue::UploadTexture(const AssetFinalizeContext& context, nvrhi::ITexture* texture, TextureFormat format,
		std::span<const GPUTextureRegion> regions, GPUUploadQueueKind queue)
	{
		ST_CORE_ASSERT(m_BatchOpen, "Uploads are recorded within a batch");
		if (!m_BatchOpen || !texture || regions.empty())
			return {};
		const auto start = std::chrono::steady_clock::now();

		// The regions one after the other in a staging buffer, their rows (of blocks) tightly packed as the data comes, each
		// region at an offset the copies accept.
		uint64_t stagingBytes = 0;
		for (const GPUTextureRegion& region : regions)
			stagingBytes = AlignUp64(stagingBytes, c_RegionAlignment) + GetRegionBytes(format, region);
		// The pool hands it out again once the GPU has copied from it.
		const uint64_t createdBefore = m_Staging.GetCreatedCount();
		nvrhi::BufferHandle staging = m_Staging.AcquireBuffer(stagingBytes, context.StagingBytes);
		const bool created = m_Staging.GetCreatedCount() != createdBefore;
		auto* destination = staging ? static_cast<uint8_t*>(m_Device->mapBuffer(staging, nvrhi::CpuAccessMode::Write)) : nullptr;
		if (!destination)
		{
			m_Staging.Release(std::move(staging));
			return {};
		}
		const float acquireMs = MillisecondsSince(start);

		m_TextureCopies.clear();
		uint64_t offset = 0;
		uint64_t bytes = 0;
		for (const GPUTextureRegion& region : regions)
		{
			offset = AlignUp64(offset, c_RegionAlignment);
			const uint64_t regionBytes = GetRegionBytes(format, region);
			AddCopy(destination + offset, region.Data, static_cast<size_t>(regionBytes));
			m_TextureCopies.push_back(GPUBufferTextureCopy { staging, offset, texture, region.MipLevel, region.FirstRow, region.Width, region.Rows });
			offset += regionBytes;
			bytes += regionBytes;
		}
		RunCopies(!created);
		m_Device->unmapBuffer(staging);

		nvrhi::ICommandList* commandList = GetList(queue);
		if (queue == GPUUploadQueueKind::Transfer)
		{
			if (std::none_of(m_TransferTextures.begin(), m_TransferTextures.end(), [texture](const nvrhi::TextureHandle& written) { return written.Get() == texture; }))
				m_TransferTextures.push_back(texture);
			// On the transfer queue the levels' states are told to NVRHI's tracking: undefined before a level's first copy (it
			// is then made a copy destination, which the transfer queue can do), a copy destination after it, whatever earlier
			// batches left in the tracking (see SubmitBatch).
			for (const GPUTextureRegion& region : regions)
			{
				commandList->beginTrackingTextureState(texture, nvrhi::TextureSubresourceSet(region.MipLevel, 1, 0, 1),
					region.FirstRow == 0 ? nvrhi::ResourceStates::Common : nvrhi::ResourceStates::CopyDest);
			}
		}
		m_GraphicsDevice.RecordBufferToTextureCopies(commandList, m_TextureCopies);
		// The command list does not reference what the raw copies write: the batch keeps it until the GPU has run them.
		if (m_BatchTextures.empty() || m_BatchTextures.back().Get() != texture)
			m_BatchTextures.push_back(texture);
		m_Staging.Release(std::move(staging), ToNvrhiQueue(queue));
		const float milliseconds = MillisecondsSince(start);
		RecordStep(context, bytes, milliseconds, acquireMs, created, true, queue);
		return GPUUploadStep { true, bytes, milliseconds };
	}

	GPUUploadStep GPUUploadQueue::UploadBuffer(const AssetFinalizeContext& context, std::span<const GPUBufferRegion> regions, GPUUploadQueueKind queue)
	{
		ST_CORE_ASSERT(m_BatchOpen, "Uploads are recorded within a batch");
		if (!m_BatchOpen || regions.empty())
			return {};
		const auto start = std::chrono::steady_clock::now();
		uint64_t bytes = 0;
		for (const GPUBufferRegion& region : regions)
			bytes += region.Size;

		const auto acquireStart = std::chrono::steady_clock::now();
		const uint64_t createdBefore = m_Staging.GetCreatedCount();
		nvrhi::BufferHandle staging = m_Staging.AcquireBuffer(bytes, context.StagingBytes);
		const bool created = m_Staging.GetCreatedCount() != createdBefore;
		auto* destination = staging ? static_cast<uint8_t*>(m_Device->mapBuffer(staging, nvrhi::CpuAccessMode::Write)) : nullptr;
		if (!destination)
		{
			m_Staging.Release(std::move(staging));
			return {};
		}
		const float acquireMs = MillisecondsSince(acquireStart);
		// One copy per range, from consecutive bytes of the staging buffer.
		uint64_t offset = 0;
		for (const GPUBufferRegion& region : regions)
		{
			AddCopy(destination + offset, region.Data, static_cast<size_t>(region.Size));
			offset += region.Size;
		}
		RunCopies(!created);
		m_Device->unmapBuffer(staging);

		nvrhi::ICommandList* commandList = GetList(queue);
		const bool transfer = queue == GPUUploadQueueKind::Transfer;
		offset = 0;
		for (const GPUBufferRegion& region : regions)
		{
			// On the transfer queue the buffer is a copy destination to NVRHI's tracking (buffers have no layout to change):
			// it would otherwise make it one from its initial state, which only the graphics queue can.
			if (transfer)
			{
				commandList->beginTrackingBufferState(region.Buffer, nvrhi::ResourceStates::CopyDest);
				const nvrhi::IBuffer* buffer = region.Buffer;
				if (std::none_of(m_TransferBuffers.begin(), m_TransferBuffers.end(), [buffer](const nvrhi::BufferHandle& written) { return written.Get() == buffer; }))
					m_TransferBuffers.push_back(region.Buffer);
			}
			commandList->copyBuffer(region.Buffer, region.Offset, staging, offset, region.Size);
			offset += region.Size;
		}
		m_Staging.Release(std::move(staging), ToNvrhiQueue(queue));
		const float milliseconds = MillisecondsSince(start);
		RecordStep(context, bytes, milliseconds, acquireMs, created, false, queue);
		return GPUUploadStep { true, bytes, milliseconds };
	}

	GPUUploadStep GPUUploadQueue::RecordSceneUpload(nvrhi::ICommandList* commandList, std::span<const GPUBufferRegion> regions)
	{
		ST_PROFILE_FUNCTION();
		GPUUploadStep result { true, 0, 0.0f };
		const auto start = std::chrono::steady_clock::now();
		size_t region = 0;
		uint64_t regionOffset = 0; // Into the current region: large regions are split over steps
		while (region < regions.size())
		{
			// A step: the next regions (or pieces of them) up to c_SceneStagingBytes, through one staging buffer.
			uint64_t stepBytes = 0;
			size_t endRegion = region;
			uint64_t endOffset = regionOffset;
			while (endRegion < regions.size() && stepBytes < c_SceneStagingBytes)
			{
				const uint64_t take = std::min(regions[endRegion].Size - endOffset, c_SceneStagingBytes - stepBytes);
				stepBytes += take;
				endOffset += take;
				if (endOffset == regions[endRegion].Size)
				{
					endRegion++;
					endOffset = 0;
				}
			}
			if (stepBytes == 0)
			{
				// Empty regions only.
				region = endRegion;
				regionOffset = endOffset;
				continue;
			}

			const uint64_t createdBefore = m_Staging.GetCreatedCount();
			nvrhi::BufferHandle staging = m_Staging.AcquireBuffer(stepBytes);
			const bool created = m_Staging.GetCreatedCount() != createdBefore;
			auto* destination = staging ? static_cast<uint8_t*>(m_Device->mapBuffer(staging, nvrhi::CpuAccessMode::Write)) : nullptr;
			if (!destination)
			{
				m_Staging.Release(std::move(staging));
				result.Recorded = false;
				break;
			}
			m_SceneCopies.clear();
			uint64_t stagingOffset = 0;
			while (region < endRegion || (region == endRegion && regionOffset < endOffset))
			{
				const GPUBufferRegion& source = regions[region];
				const uint64_t until = region < endRegion ? source.Size : endOffset;
				const uint64_t size = until - regionOffset;
				if (size > 0)
				{
					AddCopy(destination + stagingOffset, source.Data + regionOffset, static_cast<size_t>(size));
					m_SceneCopies.push_back(SceneCopy { source.Buffer, source.Offset + regionOffset, stagingOffset, size });
					stagingOffset += size;
				}
				if (region < endRegion)
				{
					region++;
					regionOffset = 0;
				}
				else
				{
					regionOffset = endOffset;
				}
			}
			RunCopies(!created);
			m_Device->unmapBuffer(staging);
			for (const SceneCopy& copy : m_SceneCopies)
				commandList->copyBuffer(copy.Buffer, copy.Offset, staging, copy.StagingOffset, copy.Size);
			m_Staging.Release(std::move(staging));
			result.Bytes += stepBytes;
			m_Stats.FrameSteps++;
			m_Stats.BufferSteps++;
		}
		result.Milliseconds = MillisecondsSince(start);
		m_Stats.FrameBytes += result.Bytes;
		m_Stats.FrameMs += result.Milliseconds;
		m_Stats.BufferBytes += result.Bytes;
		m_Stats.SceneDataBytes += result.Bytes;
		m_Stats.SceneDataMs += result.Milliseconds;
		return result;
	}

	void GPUUploadQueue::AddCopy(uint8_t* destination, const uint8_t* source, size_t bytes)
	{
		// Long runs in pieces, so that the threads share them evenly.
		while (bytes > 0)
		{
			const size_t piece = std::min(bytes, c_CopyPieceBytes);
			m_Copies.push_back(CopyRun { destination, source, piece });
			m_CopyBytes += piece;
			destination += piece;
			source += piece;
			bytes -= piece;
		}
	}

	void GPUUploadQueue::RunCopies(bool spread)
	{
		if (!spread || m_CopyBytes < c_ParallelCopyBytes || m_Copies.size() < 2)
		{
			for (const CopyRun& copy : m_Copies)
				std::memcpy(copy.Destination, copy.Source, copy.Bytes);
		}
		else
		{
			// Batches of about a piece's bytes (runs of short rows go together).
			const size_t averageBytes = std::max<size_t>(m_CopyBytes / m_Copies.size(), 1);
			const uint32_t runsPerBatch = static_cast<uint32_t>(std::max<size_t>(c_CopyPieceBytes / averageBytes, 1));
			JobSystem::ParallelFor(static_cast<uint32_t>(m_Copies.size()), runsPerBatch, [this](uint32_t begin, uint32_t end)
			{
				for (uint32_t index = begin; index < end; index++)
					std::memcpy(m_Copies[index].Destination, m_Copies[index].Source, m_Copies[index].Bytes);
			});
		}
		m_Copies.clear();
		m_CopyBytes = 0;
	}

	void GPUUploadQueue::CopyTextureLevels(nvrhi::ITexture* destination, uint32_t destinationMip, nvrhi::ITexture* source, uint32_t sourceMip, uint32_t levels)
	{
		ST_CORE_ASSERT(m_BatchOpen, "Copies are recorded within a batch");
		if (!m_BatchOpen || !destination || !source)
			return;
		for (uint32_t level = 0; level < levels; level++)
		{
			const nvrhi::TextureDesc& desc = source->getDesc();
			const uint32_t width = std::max(1u, desc.width >> (sourceMip + level));
			const uint32_t height = std::max(1u, desc.height >> (sourceMip + level));
			GetList(GPUUploadQueueKind::Graphics)->copyTexture(destination, nvrhi::TextureSlice().setMipLevel(destinationMip + level).setSize(width, height, 1), source,
				nvrhi::TextureSlice().setMipLevel(sourceMip + level).setSize(width, height, 1));
			m_Stats.TextureCopies++;
		}
	}

	GPUUploadTicket GPUUploadQueue::ReleaseToGraphics(const GPUQueueTransfer& transfer)
	{
		ST_CORE_ASSERT(m_BatchOpen, "Queue transfers are recorded within a batch");
		m_GraphicsDevice.RecordQueueTransfer(GetList(GPUUploadQueueKind::Transfer), transfer, false);
		return m_BatchTicket;
	}

	void GPUUploadQueue::AcquireOnGraphics(const GPUQueueTransfer& transfer)
	{
		ST_CORE_ASSERT(m_BatchOpen, "Queue transfers are recorded within a batch");
		m_GraphicsDevice.RecordQueueTransfer(GetList(GPUUploadQueueKind::Graphics), transfer, true);
	}

	void GPUUploadQueue::RecordStep(const AssetFinalizeContext& context, uint64_t bytes, float milliseconds, float acquireMs, bool created, bool texture,
		GPUUploadQueueKind queue)
	{
		if (context.UploadedBytes)
			*context.UploadedBytes += bytes;
		if (context.UploadMs)
			*context.UploadMs += milliseconds;
		m_Stats.FrameBytes += bytes;
		m_Stats.FrameMs += milliseconds;
		m_Stats.FrameSteps++;
		(texture ? m_Stats.TextureSteps : m_Stats.BufferSteps)++;
		(texture ? m_Stats.TextureBytes : m_Stats.BufferBytes) += bytes;
		if (queue == GPUUploadQueueKind::Transfer)
			m_Stats.TransferSteps++;
		// A creation that took long counts at once, so the next steps that create staging plan with it.
		if (created)
			m_Stats.StagingCreateMs = std::max(acquireMs, 0.75f * m_Stats.StagingCreateMs + 0.25f * acquireMs);
		// Steps too short to time say nothing about the speed.
		const float writeMs = milliseconds - acquireMs;
		if (writeMs > 0.01f && bytes >= 16 * 1024)
		{
			float& speed = created ? m_Stats.ColdBytesPerMs : m_Stats.BytesPerMs;
			speed = 0.8f * speed + 0.2f * (static_cast<float>(bytes) / writeMs);
		}
	}

	GPUUploadTicket GPUUploadQueue::GetTicket() const
	{
		// An open batch that recorded nothing yet has no ticket of its own.
		return m_BatchOpen && m_GraphicsOpen ? m_BatchTicket : m_BatchTicket - 1;
	}

	void GPUUploadQueue::CollectCompleted()
	{
		while (!m_Submissions.empty())
		{
			Submission& submission = m_Submissions.front();
			if (!m_Device->pollEventQuery(submission.Graphics) || (submission.Transfer && !m_Device->pollEventQuery(submission.Transfer)))
				break;
			m_CompletedTicket = submission.Ticket;
			m_IdleQueries.push_back(std::move(submission.Graphics));
			if (submission.Transfer)
				m_IdleQueries.push_back(std::move(submission.Transfer));
			m_Submissions.pop_front();
		}
	}

	bool GPUUploadQueue::IsComplete(GPUUploadTicket ticket)
	{
		if (ticket <= m_CompletedTicket)
			return true;
		CollectCompleted();
		return ticket <= m_CompletedTicket;
	}

	void GPUUploadQueue::WaitForTicket(GPUUploadTicket ticket)
	{
		if (IsComplete(ticket))
			return;
		if (m_BatchOpen && ticket >= m_BatchTicket)
		{
			// The ticket's work is still being recorded: submit it, and go on in a new batch.
			SubmitBatch();
			BeginBatch();
		}
		for (Submission& submission : m_Submissions)
		{
			if (submission.Ticket > ticket)
				break;
			m_Device->waitEventQuery(submission.Graphics);
			if (submission.Transfer)
				m_Device->waitEventQuery(submission.Transfer);
		}
		CollectCompleted();
	}

}
