#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/StateRecorder.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Strata
{

	// Jolt's simulation state (PhysicsSystem::SaveState, CharacterVirtual::SaveState) in memory, for moving it from one Jolt
	// system to another (a world that grows, see PhysicsWorld): written once, then read once from the start. Jolt's own
	// StateRecorderImpl goes through a string stream, which is several times slower for the hundreds of thousands of bodies
	// a world moves. Internal to the Physics module.
	class PhysicsStateBuffer final : public JPH::StateRecorder
	{
	public:
		void WriteBytes(const void* data, size_t size) override
		{
			const std::byte* bytes = static_cast<const std::byte*>(data);
			m_Bytes.insert(m_Bytes.end(), bytes, bytes + size);
		}

		void ReadBytes(void* data, size_t size) override
		{
			if (size > m_Bytes.size() - m_ReadOffset)
			{
				// Jolt reads what it wrote, so this is a bug; the caller sees IsFailed and the reader gets zeros.
				m_Failed = true;
				std::memset(data, 0, size);
				m_ReadOffset = m_Bytes.size();
				return;
			}
			std::memcpy(data, m_Bytes.data() + m_ReadOffset, size);
			m_ReadOffset += size;
		}

		bool IsEOF() const override { return m_ReadOffset >= m_Bytes.size(); }
		bool IsFailed() const override { return m_Failed; }

		size_t GetSize() const { return m_Bytes.size(); }
		void Clear()
		{
			m_Bytes.clear();
			m_ReadOffset = 0;
			m_Failed = false;
		}
	private:
		std::vector<std::byte> m_Bytes;
		size_t m_ReadOffset = 0;
		bool m_Failed = false;
	};

}
