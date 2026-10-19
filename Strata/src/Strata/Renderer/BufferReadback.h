#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

namespace Strata
{

	// Copies of a buffer range to the CPU, read without stalling a frame (like TextureReadback, for buffers): Record copies
	// the range in a command list, Submit marks it once that list was executed, IsReady polls (never waits) whether the
	// GPU has run it, and Read takes the bytes. A readback is reused: each Record replaces the previous copy. Main thread
	// only.
	class BufferReadback
	{
	public:
		// A readback of up to `capacity` bytes; IsValid is false when the staging buffer could not be created (logged).
		BufferReadback(nvrhi::IDevice* device, uint64_t capacity, const std::string& debugName);

		BufferReadback(const BufferReadback&) = delete;
		BufferReadback& operator=(const BufferReadback&) = delete;

		bool IsValid() const { return m_Staging != nullptr; }
		uint64_t GetCapacity() const { return m_Capacity; }

		// Records the copy of [offset, offset + bytes) of `source` (bytes at most the capacity) into the command list.
		void Record(nvrhi::ICommandList* commandList, nvrhi::IBuffer* source, uint64_t offset, uint64_t bytes);
		// After the command list with the copy was executed: the copy is pending until the GPU has run it.
		void Submit();
		bool IsPending() const { return m_Pending; }
		// Whether the submitted copy completed (polled).
		bool IsReady() const;
		// Blocks until the submitted copy completed (tests and tools).
		void Wait();
		// The copied bytes from `offset` on (at most what was recorded) once ready; false otherwise, when the staging cannot be
		// mapped, or when `offset` lies beyond what was recorded.
		bool Read(void* destination, uint64_t bytes, uint64_t offset = 0) const;
	private:
		nvrhi::IDevice* m_Device;
		nvrhi::BufferHandle m_Staging;
		nvrhi::EventQueryHandle m_Query;
		uint64_t m_Capacity = 0;
		uint64_t m_RecordedBytes = 0;
		bool m_Recorded = false;
		bool m_Pending = false;
	};

}
