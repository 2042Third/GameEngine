#pragma once

#include "Strata/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace Strata
{

	enum class BoundedReadStatus : uint8_t
	{
		Complete, // The whole source was read
		TooLarge, // The source holds more than maxSize bytes
		Failed    // The read function failed
	};

	// Reads a source (e.g. an open file) to its end, keeping at most maxSize bytes in memory.
	class BoundedRead
	{
	public:
		// Reads into buffer and returns the number of bytes read: 0 at the end of the source, nullopt on failure.
		using ReadFunction = std::function<std::optional<size_t>(std::span<char> buffer)>;

		// Calls read until the source ends. sizeHint (e.g. the size the file system reported) only sizes the first
		// buffer: a file that grew or shrank since it was inspected is still read to its end, so the result is
		// never a prefix cut at a stale size. contents holds the data on Complete and is empty otherwise.
		static BoundedReadStatus ReadAll(const ReadFunction& read, size_t sizeHint, size_t maxSize, std::string& contents);
	};

}
