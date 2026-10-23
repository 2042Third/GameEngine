#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace Strata
{

	// The physical atmosphere of SkyLightSource::Atmosphere: Earth's, with its air and aerosols scaled by the sky light's
	// settings. A planet whose ground is the world's y = 0 plane, lengths in kilometers, scattering and absorption
	// coefficients per kilometer for the red, green and blue primaries. Include/Atmosphere.glsl mirrors these constants
	// and the transmittance's quadrature and lookup-table mapping: the GPU fills its table with them, and the CPU evaluates
	// the same quadrature in double (ComputeTransmittance) for the sun's light at the ground.
	namespace Atmosphere
	{

		constexpr double c_BottomRadius = 6360.0; // The ground
		constexpr double c_TopRadius = 6460.0;    // Where the air ends
		// Air (Rayleigh scattering) and aerosols (Mie scattering) thin out exponentially with height; ozone absorbs in a layer
		// whose density rises linearly to its peak at c_OzoneCenterHeight and falls again, c_OzoneHalfWidth either way.
		constexpr double c_RayleighScaleHeight = 8.0;
		constexpr double c_MieScaleHeight = 1.2;
		constexpr double c_OzoneCenterHeight = 25.0;
		constexpr double c_OzoneHalfWidth = 15.0;
		// At sea level, per kilometer (red, green, blue).
		constexpr double c_RayleighScattering[3] = { 5.802e-3, 13.558e-3, 33.1e-3 };
		constexpr double c_MieScattering = 3.996e-3;
		constexpr double c_MieAbsorption = 0.444e-3;
		constexpr double c_OzoneAbsorption[3] = { 0.650e-3, 1.881e-3, 0.085e-3 };
		// The aerosols' forward scattering (Henyey-Greenstein asymmetry).
		constexpr double c_MieAsymmetry = 0.8;

		// The transmittance's quadrature: midpoint steps from a point to the top of the atmosphere.
		constexpr uint32_t c_TransmittanceSteps = 40;
		// Lookup tables (RGBA16F): transmittance by height and direction, the light of all higher scattering orders by height
		// and the sun's height, and the sky seen from the viewer by direction (Include/Atmosphere.glsl maps them).
		constexpr uint32_t c_TransmittanceWidth = 256;
		constexpr uint32_t c_TransmittanceHeight = 64;
		constexpr uint32_t c_MultipleScatteringSize = 32;
		constexpr uint32_t c_SkyViewWidth = 192;
		constexpr uint32_t c_SkyViewHeight = 108;

		// The medium's coefficients for the sky light's settings: Density scales the air and the aerosols (and the ozone in the
		// air), Haze the aerosols once more. Negative or non-finite settings count as 0.
		struct Medium
		{
			glm::dvec3 RayleighScattering = glm::dvec3(0.0);
			glm::dvec3 MieScattering = glm::dvec3(0.0);
			glm::dvec3 MieExtinction = glm::dvec3(0.0);
			glm::dvec3 OzoneAbsorption = glm::dvec3(0.0);
		};
		Medium GetMedium(float density, float haze);

		// The point (its distance r from the planet's center) and direction (mu, the cosine of its angle with the zenith) a
		// texel center of the transmittance table stands for, at uv in [0, 1]^2.
		glm::dvec2 GetTransmittanceRadiusMu(const glm::dvec2& uv);
		// The transmittance from a point at distance `radius` from the center along a direction of cosine `mu` with the
		// zenith to the top of the atmosphere, as the table stores it: the ground is ignored (callers test it), and heights
		// below it count as the ground's.
		glm::dvec3 ComputeTransmittance(const Medium& medium, double radius, double mu);
		// The fraction of a sun's light that reaches the ground through the atmosphere, toward `towardSun` (a unit vector),
		// for a disk of `angularRadius` radians: its center's transmittance, times the share of the disk above the horizon.
		glm::dvec3 ComputeSunTransmittance(const Medium& medium, const glm::dvec3& towardSun, double angularRadius);

	}

}
