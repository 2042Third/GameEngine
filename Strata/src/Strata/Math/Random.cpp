#include "stpch.h"
#include "Strata/Math/Random.h"

#include <random>

namespace Strata
{

	static std::mt19937_64& GetEngine()
	{
		thread_local std::mt19937_64 t_Engine = []
		{
			std::random_device device;
			std::seed_seq seed { device(), device(), device(), device() };
			return std::mt19937_64(seed);
		}();
		return t_Engine;
	}

	void Random::Seed(uint64_t seed)
	{
		GetEngine().seed(seed);
	}

	float Random::Float()
	{
		// 24 random mantissa bits give uniformly spaced values in [0, 1).
		return static_cast<float>(GetEngine()() >> 40) * (1.0f / 16777216.0f);
	}

	float Random::Float(float min, float max)
	{
		return min + (max - min) * Float();
	}

	int32_t Random::Int(int32_t min, int32_t max)
	{
		if (min > max)
			std::swap(min, max);
		std::uniform_int_distribution<int32_t> distribution(min, max);
		return distribution(GetEngine());
	}

	uint64_t Random::UInt64()
	{
		return GetEngine()();
	}

	glm::vec3 Random::Vec3(float min, float max)
	{
		return glm::vec3(Float(min, max), Float(min, max), Float(min, max));
	}

	glm::vec3 Random::UnitVector()
	{
		// Uniform on the sphere: uniform z and uniform angle around it.
		const float z = Float(-1.0f, 1.0f);
		const float angle = Float(0.0f, glm::two_pi<float>());
		const float radius = glm::sqrt(glm::max(0.0f, 1.0f - z * z));
		return glm::vec3(radius * glm::cos(angle), radius * glm::sin(angle), z);
	}

}
