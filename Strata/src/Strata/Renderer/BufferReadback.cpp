#include "stpch.h"
#include "Strata/Renderer/BufferReadback.h"

#include "Strata/Core/Assert.h"

#include <algorithm>
#include <cstring>

namespace Strata
{

	BufferReadback::BufferReadback(nvrhi::IDevice* device, uint64_t capacity, const std::string& debugName)
		: m_Device(device), m_Capacity(capacity)
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = std::max<uint64_t>(capacity, 4);
		desc.cpuAccess = nvrhi::CpuAccessMode::Read;
		desc.debugName = debugName;
		desc.initialState = nvrhi::ResourceStates::CopyDest;
		desc.keepInitialState = true;
		m_Staging = m_Device->createBuffer(desc);
		if (!m_Staging)
			ST_CORE_ERROR("BufferReadback: failed to create the staging buffer of '{}' ({} bytes)", debugName, desc.byteSize);
		m_Query = m_Device->createEventQuery();
	}

	void BufferReadback::Record(nvrhi::ICommandList* commandList, nvrhi::IBuffer* source, uint64_t offset, uint64_t bytes)
	{
		ST_CORE_ASSERT(bytes <= m_Capacity, "Recording {} bytes into a readback of {}", bytes, m_Capacity);
		if (!m_Staging || !source || bytes == 0)
			return;
		bytes = std::min(bytes, m_Capacity);
		commandList->copyBuffer(m_Staging, 0, source, offset, bytes);
		m_RecordedBytes = bytes;
		m_Recorded = true;
		m_Pending = false;
	}

	void BufferReadback::Submit()
	{
		if (!m_Recorded)
			return;
		m_Device->resetEventQuery(m_Query);
		m_Device->setEventQuery(m_Query, nvrhi::CommandQueue::Graphics);
		m_Recorded = false;
		m_Pending = true;
	}

	bool BufferReadback::IsReady() const
	{
		return m_Pending && m_Device->pollEventQuery(m_Query);
	}

	void BufferReadback::Wait()
	{
		if (m_Pending)
			m_Device->waitEventQuery(m_Query);
	}

	bool BufferReadback::Read(void* destination, uint64_t bytes, uint64_t offset) const
	{
		if (!IsReady() || offset > m_RecordedBytes)
			return false;
		const void* mapped = m_Device->mapBuffer(m_Staging, nvrhi::CpuAccessMode::Read);
		if (!mapped)
			return false;
		std::memcpy(destination, static_cast<const uint8_t*>(mapped) + offset, static_cast<size_t>(std::min(bytes, m_RecordedBytes - offset)));
		m_Device->unmapBuffer(m_Staging);
		return true;
	}

}
