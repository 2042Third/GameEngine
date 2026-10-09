#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Strata
{

	// Thread-local pseudo-random numbers (each thread has its own generator, seeded randomly).
	// Use Seed() for reproducible sequences on the calling thread.
	class Random
	{
	public:
		static void Seed(uint64_t seed);

		static float Float();                     // [0, 1)
		static float Float(float min, float max); // [min, max)
		static int32_t Int(int32_t min, int32_t max); // [min, max] inclusive
		static uint64_t UInt64();
		static glm::vec3 Vec3(float min, float max);
		static glm::vec3 UnitVector();
	};

}
