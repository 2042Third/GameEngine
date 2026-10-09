#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Strata
{

	// A value one thread publishes and another takes without either ever waiting (a sequence lock), e.g. to hand state to
	// a real-time audio thread. The reader takes the latest complete value, or nothing while the writer is in the middle of
	// writing one (it then tries again later). One writer thread and one reader thread.
	//
	// The value is kept in relaxed atomic words, so reading it during a write is no data race; the sequence number, odd
	// while a value is being written, tells the reader whether the words it read belong to one value.
	template<typename T>
	class SequenceLockedValue
	{
		static_assert(std::is_trivially_copyable_v<T>, "SequenceLockedValue copies values bytewise");
	public:
		// Writer thread.
		void Publish(const T& value)
		{
			std::array<uint32_t, c_WordCount> words {};
			std::memcpy(words.data(), &value, sizeof(T));

			const uint64_t sequence = m_Sequence.load(std::memory_order_relaxed);
			m_Sequence.store(sequence + 1, std::memory_order_relaxed);
			// The odd sequence becomes visible before any word of the new value.
			std::atomic_thread_fence(std::memory_order_release);
			for (size_t index = 0; index < c_WordCount; index++)
				m_Words[index].store(words[index], std::memory_order_relaxed);
			m_Sequence.store(sequence + 2, std::memory_order_release);
		}

		// Reader thread: the latest value, if one was published since the last value taken and none is being written.
		bool TakeNew(T& outValue)
		{
			const uint64_t sequence = m_Sequence.load(std::memory_order_acquire);
			if (sequence == m_TakenSequence || (sequence & 1u) != 0)
				return false;

			std::array<uint32_t, c_WordCount> words;
			for (size_t index = 0; index < c_WordCount; index++)
				words[index] = m_Words[index].load(std::memory_order_relaxed);
			// The words are read before the sequence is checked again.
			std::atomic_thread_fence(std::memory_order_acquire);
			if (m_Sequence.load(std::memory_order_relaxed) != sequence)
				return false; // Rewritten meanwhile

			// T is trivially copyable (asserted above), so copying its bytes is valid even when it has default member
			// initializers; the void* tells GCC so (-Wclass-memaccess).
			std::memcpy(static_cast<void*>(&outValue), words.data(), sizeof(T));
			m_TakenSequence = sequence;
			return true;
		}
	private:
		static constexpr size_t c_WordCount = (sizeof(T) + sizeof(uint32_t) - 1) / sizeof(uint32_t);

		std::atomic<uint64_t> m_Sequence = 0;
		std::array<std::atomic<uint32_t>, c_WordCount> m_Words {};
		uint64_t m_TakenSequence = 0; // Reader thread only
	};

}
