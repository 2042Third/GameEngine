#pragma once

#include "Strata/Core/Base.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Strata
{

	// Little-endian binary serialization for cooked asset formats. All supported platforms are little-endian; the
	// formats record this by convention and readers validate sizes, never trusting lengths from the data.
	class BinaryWriter
	{
	public:
		template<typename T>
		void Write(const T& value)
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryWriter::Write requires trivially copyable data");
			const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
			m_Data.insert(m_Data.end(), bytes, bytes + sizeof(T));
		}

		template<typename T>
		void WriteArray(std::span<const T> values)
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryWriter::WriteArray requires trivially copyable data");
			Write(static_cast<uint64_t>(values.size()));
			WriteBytes(values.data(), values.size_bytes());
		}

		void WriteString(std::string_view text)
		{
			Write(static_cast<uint32_t>(text.size()));
			WriteBytes(text.data(), text.size());
		}

		void WriteBytes(const void* data, size_t size)
		{
			if (size == 0)
				return;
			const auto* bytes = static_cast<const uint8_t*>(data);
			m_Data.insert(m_Data.end(), bytes, bytes + size);
		}

		size_t GetSize() const { return m_Data.size(); }
		const std::vector<uint8_t>& GetData() const { return m_Data; }
		std::vector<uint8_t>&& TakeData() { return std::move(m_Data); }
	private:
		std::vector<uint8_t> m_Data;
	};

	// Bounds-checked reader. Any read past the end puts the reader into a failed state; further reads return
	// zero-initialized values. Check IsValid() after reading a structure.
	class BinaryReader
	{
	public:
		explicit BinaryReader(std::span<const uint8_t> data)
			: m_Data(data)
		{
		}

		template<typename T>
		T Read()
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::Read requires trivially copyable data");
			T value {};
			ReadBytes(&value, sizeof(T));
			return value;
		}

		// Reads an array written by BinaryWriter::WriteArray. maxCount guards against corrupt lengths.
		template<typename T>
		bool ReadArray(std::vector<T>& outValues, uint64_t maxCount = UINT32_MAX)
		{
			static_assert(std::is_trivially_copyable_v<T>, "BinaryReader::ReadArray requires trivially copyable data");
			const uint64_t count = Read<uint64_t>();
			if (!m_Valid || count > maxCount || count > GetRemaining() / sizeof(T))
			{
				m_Valid = false;
				return false;
			}
			outValues.resize(static_cast<size_t>(count));
			ReadBytes(outValues.data(), static_cast<size_t>(count) * sizeof(T));
			return m_Valid;
		}

		std::string ReadString(uint32_t maxLength = 1u << 20)
		{
			const uint32_t length = Read<uint32_t>();
			if (!m_Valid || length > maxLength || length > GetRemaining())
			{
				m_Valid = false;
				return {};
			}
			std::string text(length, '\0');
			ReadBytes(text.data(), length);
			return text;
		}

		bool ReadBytes(void* destination, size_t size)
		{
			if (!m_Valid || size > GetRemaining())
			{
				m_Valid = false;
				if (size > 0)
					std::memset(destination, 0, size);
				return false;
			}
			if (size > 0)
				std::memcpy(destination, m_Data.data() + m_Position, size);
			m_Position += size;
			return true;
		}

		// View of the next `size` bytes without copying (empty and failed if out of range).
		std::span<const uint8_t> ReadView(size_t size)
		{
			if (!m_Valid || size > GetRemaining())
			{
				m_Valid = false;
				return {};
			}
			std::span<const uint8_t> view = m_Data.subspan(m_Position, size);
			m_Position += size;
			return view;
		}

		bool Seek(size_t position)
		{
			if (position > m_Data.size())
			{
				m_Valid = false;
				return false;
			}
			m_Position = position;
			return true;
		}

		bool IsValid() const { return m_Valid; }
		size_t GetPosition() const { return m_Position; }
		size_t GetRemaining() const { return m_Data.size() - m_Position; }
	private:
		std::span<const uint8_t> m_Data;
		size_t m_Position = 0;
		bool m_Valid = true;
	};

}
