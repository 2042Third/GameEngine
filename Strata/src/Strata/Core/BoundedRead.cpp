#include "stpch.h"
#include "Strata/Core/BoundedRead.h"

#include <algorithm>
#include <limits>

namespace Strata
{

	BoundedReadStatus BoundedRead::ReadAll(const ReadFunction& read, size_t sizeHint, size_t maxSize, std::string& contents)
	{
		contents.clear();

		// Reading one byte beyond the limit is enough to tell that the source is too large.
		const size_t limit = maxSize < std::numeric_limits<size_t>::max() ? maxSize + 1 : maxSize;
		contents.resize(std::min(sizeHint, limit - 1) + 1);

		size_t total = 0;
		while (true)
		{
			if (total == contents.size())
			{
				if (contents.size() >= limit)
					break;
				// Doubles the buffer, up to the limit (written so that it cannot overflow).
				const size_t grown = contents.size() <= limit - contents.size() ? contents.size() * 2 : limit;
				contents.resize(grown);
			}

			const std::optional<size_t> count = read(std::span<char>(contents.data() + total, contents.size() - total));
			if (!count || *count > contents.size() - total)
			{
				contents.clear();
				return BoundedReadStatus::Failed;
			}
			if (*count == 0)
				break;
			total += *count;
		}

		if (total > maxSize)
		{
			contents.clear();
			return BoundedReadStatus::TooLarge;
		}
		contents.resize(total);
		return BoundedReadStatus::Complete;
	}

}
