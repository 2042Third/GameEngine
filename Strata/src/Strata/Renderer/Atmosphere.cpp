#include "stpch.h"
#include "Strata/Renderer/Atmosphere.h"

#include <algorithm>
#include <cmath>

namespace Strata::Atmosphere
{

	namespace
	{

		double Positive(float value)
		{
			return std::isfinite(value) ? std::max(static_cast<double>(value), 0.0) : 0.0;
		}

		glm::dvec3 Extinction(const Medium& medium, double height)
		{
			height = std::max(height, 0.0);
			const double rayleigh = std::exp(-height / c_RayleighScaleHeight);
			const double mie = std::exp(-height / c_MieScaleHeight);
			const double ozone = std::max(1.0 - std::abs(height - c_OzoneCenterHeight) / c_OzoneHalfWidth, 0.0);
			return medium.RayleighScattering * rayleigh + medium.MieExtinction * mie + medium.OzoneAbsorption * ozone;
		}

		// Along a ray from `radius` with cosine `mu`, the distance to the top of the atmosphere; the products are formed so
		// that nothing cancels near the horizon.
		double DistanceToTop(double radius, double mu)
		{
			const double discriminant = (c_TopRadius - radius) * (c_TopRadius + radius) + radius * radius * mu * mu;
			return std::max(-radius * mu + std::sqrt(std::max(discriminant, 0.0)), 0.0);
		}

	}

	Medium GetMedium(float density, float haze)
	{
		const double air = Positive(density);
		const double aerosols = air * Positive(haze);
		Medium medium;
		medium.RayleighScattering = glm::dvec3(c_RayleighScattering[0], c_RayleighScattering[1], c_RayleighScattering[2]) * air;
		medium.MieScattering = glm::dvec3(c_MieScattering * aerosols);
		medium.MieExtinction = glm::dvec3((c_MieScattering + c_MieAbsorption) * aerosols);
		medium.OzoneAbsorption = glm::dvec3(c_OzoneAbsorption[0], c_OzoneAbsorption[1], c_OzoneAbsorption[2]) * air;
		return medium;
	}

	glm::dvec2 GetTransmittanceRadiusMu(const glm::dvec2& uv)
	{
		// Texel centers span the whole range: the first and last ones sit on its ends.
		const glm::dvec2 size(c_TransmittanceWidth, c_TransmittanceHeight);
		const glm::dvec2 unit = glm::clamp((uv - 0.5 / size) / (1.0 - 1.0 / size), 0.0, 1.0);
		// Directions crowd toward the horizon (mu = t |t|), heights toward the ground.
		const double t = 2.0 * unit.x - 1.0;
		const double mu = t * std::abs(t);
		const double radius = c_BottomRadius + unit.y * unit.y * (c_TopRadius - c_BottomRadius);
		return glm::dvec2(radius, mu);
	}

	glm::dvec3 ComputeTransmittance(const Medium& medium, double radius, double mu)
	{
		const double distance = DistanceToTop(radius, mu);
		const double step = distance / static_cast<double>(c_TransmittanceSteps);
		glm::dvec3 depth(0.0);
		for (uint32_t index = 0; index < c_TransmittanceSteps; index++)
		{
			const double t = (static_cast<double>(index) + 0.5) * step;
			const double pointRadius = std::sqrt(radius * radius + t * t + 2.0 * radius * mu * t);
			depth += Extinction(medium, pointRadius - c_BottomRadius);
		}
		return glm::exp(-depth * step);
	}

	glm::dvec3 ComputeSunTransmittance(const Medium& medium, const glm::dvec3& towardSun, double angularRadius)
	{
		// The disk's center seen from the ground; the part of the disk below the horizon (a linear share across its
		// diameter) is hidden, so the light fades out over the disk's width instead of at once.
		const double mu = std::clamp(towardSun.y, -1.0, 1.0);
		const double elevation = std::asin(mu);
		const double radius = std::max(angularRadius, 1.0e-4);
		const double visible = std::clamp((elevation + radius) / (2.0 * radius), 0.0, 1.0);
		if (visible <= 0.0)
			return glm::dvec3(0.0);
		return ComputeTransmittance(medium, c_BottomRadius, std::max(mu, 0.0)) * visible;
	}

}
