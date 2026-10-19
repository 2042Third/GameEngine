#pragma once

#include "Strata/Core/JobSystem.h"

#include <cstddef>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

namespace Strata
{

	// A copy of `source` made member-wise (by `makeCopy`, its copy constructor) with the members `members` (pointers to
	// members holding large arrays) copied in parallel on the job system: copying large arrays is mostly the system providing
	// fresh memory, which threads do side by side. The members are moved out of the source for the member-wise copy and moved
	// back once copied, so the source is as it was when this returns; a member left out of `members` is still copied, only
	// not in parallel. Nothing else may use `source` meanwhile. Call it where the members and the copy constructor are
	// accessible (the class's own member functions).
	template<typename T, typename MakeCopy, typename... Members>
	auto CloneWithParallelMembers(T& source, const MakeCopy& makeCopy, Members... members)
	{
		const std::tuple<Members...> memberPointers(members...);
		auto held = std::make_tuple(std::move(source.*members)...);
		auto clone = makeCopy();
		std::vector<std::function<void()>> copies;
		[&]<size_t... Index>(std::index_sequence<Index...>)
		{
			(copies.push_back([&clone, &held, member = std::get<Index>(memberPointers)]() { (*clone).*member = std::get<Index>(held); }), ...);
		}(std::index_sequence_for<Members...>());
		JobSystem::ParallelFor(static_cast<uint32_t>(copies.size()), 1, [&copies](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
				copies[index]();
		});
		[&]<size_t... Index>(std::index_sequence<Index...>)
		{
			((source.*std::get<Index>(memberPointers) = std::move(std::get<Index>(held))), ...);
		}(std::index_sequence_for<Members...>());
		return clone;
	}

}
