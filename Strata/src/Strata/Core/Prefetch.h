#pragma once

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	#include <xmmintrin.h>
#elif defined(_MSC_VER) && defined(_M_ARM64)
	#include <intrin.h>
#endif

namespace Strata
{

	// Asks the CPU to start loading the cache line that holds `address` into all cache levels, so that a read a little
	// later does not wait for memory. It is only a hint: it never faults and has no other effect, whatever the address
	// (null, freed or not mapped), so it may name memory that the caller must not read yet. For loops over objects in
	// heap blocks of their own, whose first reads the hardware prefetchers do not reliably anticipate.
	inline void Prefetch(const void* address)
	{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
		_mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
#elif defined(_MSC_VER) && defined(_M_ARM64)
		__prefetch(address);
#elif defined(__clang__) || defined(__GNUC__)
		__builtin_prefetch(address);
#else
		static_cast<void>(address);
#endif
	}

}
