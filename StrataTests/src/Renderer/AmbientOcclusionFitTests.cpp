#include <doctest/doctest.h>

#include "Renderer/AmbientOcclusionFitReference.h"
#include "Strata/Renderer/AmbientOcclusionFit.h"

#include <cmath>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Renderer.AmbientOcclusion")
{
	TEST_CASE("The multi-bounce fit matches its radiosity reference and keeps its bounds")
	{
		// The reference's conditional means (Tools/FitAOMultiBounce.py): the fit within 0.03 of each.
		for (const AOMultiBounceSample& sample : c_AOMultiBounceReference)
		{
			CAPTURE(sample.Visibility);
			CAPTURE(sample.Albedo);
			const float fitted = AOMultiBounce(sample.Visibility, glm::vec3(sample.Albedo)).x;
			CHECK(std::abs(fitted - sample.Reference) <= 0.03f);
		}

		// Nothing occluded keeps all its light; black walls reflect nothing; walls only ever add light; more visibility and
		// brighter walls never darken.
		for (int albedoStep = 0; albedoStep <= 10; albedoStep++)
		{
			const float albedo = static_cast<float>(albedoStep) / 10.0f;
			CAPTURE(albedo);
			CHECK(AOMultiBounce(1.0f, glm::vec3(albedo)).x == doctest::Approx(1.0f));
			float previous = -1.0f;
			for (int visibilityStep = 0; visibilityStep <= 20; visibilityStep++)
			{
				const float visibility = static_cast<float>(visibilityStep) / 20.0f;
				CAPTURE(visibility);
				const float value = AOMultiBounce(visibility, glm::vec3(albedo)).x;
				CHECK(std::isfinite(value));
				CHECK(value >= visibility - 1e-6f);
				CHECK(value <= 1.0f + 1e-6f);
				CHECK(value >= previous - 1e-6f);
				previous = value;
				if (albedoStep > 0)
					CHECK(value >= AOMultiBounce(visibility, glm::vec3(albedo - 0.1f)).x - 1e-6f);
				if (albedoStep == 0)
					CHECK(value == doctest::Approx(visibility));
			}
		}
		// Each channel of a colored albedo on its own.
		const glm::vec3 colored = AOMultiBounce(0.5f, glm::vec3(0.8f, 0.4f, 0.1f));
		CHECK(colored.x > colored.y);
		CHECK(colored.y > colored.z);
		CHECK(colored.z == doctest::Approx(AOMultiBounce(0.5f, glm::vec3(0.1f)).x));
	}
}
