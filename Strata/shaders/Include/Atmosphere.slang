#ifndef STRATA_ATMOSPHERE_SLANG
#define STRATA_ATMOSPHERE_SLANG

#include "Include/Common.slang"

// The physical atmosphere of SkyLightSource::Atmosphere (SceneRenderer, the IBL/Atmosphere*.comp passes). Keep the constants,
// the transmittance's quadrature and the table mappings in sync with Renderer/Atmosphere.h: the CPU evaluates the same
// transmittance for the sun's light at the ground. A planet whose ground is the world's y = 0 plane; lengths in kilometers,
// coefficients per kilometer.

static const float c_AtmosphereBottomRadius = 6360.0;
static const float c_AtmosphereTopRadius = 6460.0;
static const float c_RayleighScaleHeight = 8.0;
static const float c_MieScaleHeight = 1.2;
static const float c_OzoneCenterHeight = 25.0;
static const float c_OzoneHalfWidth = 15.0;
static const float c_MieAsymmetry = 0.8;
static const uint c_TransmittanceSteps = 40u;

#define ST_TRANSMITTANCE_SIZE         float2(256.0, 64.0)
#define ST_MULTIPLE_SCATTERING_SIZE   float2(32.0, 32.0)
#define ST_SKY_VIEW_SIZE              float2(192.0, 108.0)

// The tables that hold light per lux of sun (multiple scattering, sky view, aerial perspective's in-scattering, the environment
// made from the sky view) store it times this (Atmosphere::c_StorageScale): after sunset the sky per lux falls below the
// smallest normal half float, where a smooth gradient turns into a few levels (bands). A power of two is exact in half floats,
// so values that were normal before are the same; readers multiply by EnvironmentParams.x, which divides by it.
#define ST_ATMOSPHERE_STORAGE_SCALE   16384.0

// The medium's coefficients (RenderData::AtmosphereConstants, scaled by the sky light's density and haze).
struct AtmosphereMedium
{
	float3 RayleighScattering;
	float3 MieScattering;
	float3 MieExtinction;
	float3 OzoneAbsorption;
};

float3 GetAtmosphereExtinction(AtmosphereMedium medium, float height)
{
	height = max(height, 0.0);
	float rayleigh = exp(-height / c_RayleighScaleHeight);
	float mie = exp(-height / c_MieScaleHeight);
	float ozone = max(1.0 - abs(height - c_OzoneCenterHeight) / c_OzoneHalfWidth, 0.0);
	return medium.RayleighScattering * rayleigh + medium.MieExtinction * mie + medium.OzoneAbsorption * ozone;
}

// Along a ray from `radius` with cosine `mu` to the zenith: the distance to the top of the atmosphere, whether it meets the
// ground and the distance to it. The products are formed so that nothing cancels near the horizon.
float DistanceToAtmosphereTop(float radius, float mu)
{
	float discriminant = (c_AtmosphereTopRadius - radius) * (c_AtmosphereTopRadius + radius) + radius * radius * mu * mu;
	return max(-radius * mu + sqrt(max(discriminant, 0.0)), 0.0);
}

bool RayMeetsGround(float radius, float mu)
{
	return mu < 0.0 && radius * radius * mu * mu - (radius - c_AtmosphereBottomRadius) * (radius + c_AtmosphereBottomRadius) >= 0.0;
}

float DistanceToGround(float radius, float mu)
{
	float discriminant = radius * radius * mu * mu - (radius - c_AtmosphereBottomRadius) * (radius + c_AtmosphereBottomRadius);
	return max(-radius * mu - sqrt(max(discriminant, 0.0)), 0.0);
}

// The distance from the center of a point `t` along the ray.
float RadiusAlongRay(float radius, float mu, float t)
{
	return sqrt(max(radius * radius + t * t + 2.0 * radius * mu * t, 0.0));
}

// Transmittance to the top of the atmosphere, ignoring the ground: Atmosphere::ComputeTransmittance.
float3 ComputeTransmittance(AtmosphereMedium medium, float radius, float mu)
{
	float step = DistanceToAtmosphereTop(radius, mu) / float(c_TransmittanceSteps);
	float3 depth = float3(0.0);
	for (uint index = 0u; index < c_TransmittanceSteps; index++)
	{
		float t = (float(index) + 0.5) * step;
		depth += GetAtmosphereExtinction(medium, RadiusAlongRay(radius, mu, t) - c_AtmosphereBottomRadius);
	}
	return exp(-depth * step);
}

// Texel centers span each table's whole range: the first and last ones sit on its ends.
float2 TableUVToUnit(float2 uv, float2 size)
{
	return clamp((uv - 0.5 / size) / (1.0 - 1.0 / size), 0.0, 1.0);
}

float2 UnitToTableUV(float2 unit, float2 size)
{
	return unit * (1.0 - 1.0 / size) + 0.5 / size;
}

// The transmittance table: directions crowd toward the horizon (mu = t |t|), heights toward the ground (Atmosphere::
// GetTransmittanceRadiusMu).
float2 TransmittanceUVToRadiusMu(float2 uv)
{
	float2 unit = TableUVToUnit(uv, ST_TRANSMITTANCE_SIZE);
	float t = 2.0 * unit.x - 1.0;
	return float2(c_AtmosphereBottomRadius + unit.y * unit.y * (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), t * abs(t));
}

float2 RadiusMuToTransmittanceUV(float radius, float mu)
{
	float height = clamp((radius - c_AtmosphereBottomRadius) / (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), 0.0, 1.0);
	float2 unit = float2(0.5 + 0.5 * sign(mu) * sqrt(abs(clamp(mu, -1.0, 1.0))), sqrt(height));
	return UnitToTableUV(unit, ST_TRANSMITTANCE_SIZE);
}

// The multiple-scattering table: by the sun's cosine to the zenith and the height.
float2 MultipleScatteringUVToRadiusMu(float2 uv)
{
	float2 unit = TableUVToUnit(uv, ST_MULTIPLE_SCATTERING_SIZE);
	return float2(c_AtmosphereBottomRadius + unit.y * (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), unit.x * 2.0 - 1.0);
}

float2 RadiusMuToMultipleScatteringUV(float radius, float mu)
{
	float height = clamp((radius - c_AtmosphereBottomRadius) / (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), 0.0, 1.0);
	return UnitToTableUV(float2(clamp(mu, -1.0, 1.0) * 0.5 + 0.5, height), ST_MULTIPLE_SCATTERING_SIZE);
}

// The sky-view table: around the world's vertical axis (u, from +X toward +Z) and by elevation above the viewer's horizontal
// plane (v from straight down to straight up), its rows crowding toward the horizon (elevation = t |t| pi / 2). u wraps.
float3 SkyViewUVToDirection(float2 uv)
{
	float azimuth = uv.x * TWO_PI;
	float t = 2.0 * TableUVToUnit(uv, ST_SKY_VIEW_SIZE).y - 1.0;
	float elevation = t * abs(t) * (0.5 * PI);
	float horizontal = cos(elevation);
	return float3(cos(azimuth) * horizontal, sin(elevation), sin(azimuth) * horizontal);
}

float2 DirectionToSkyViewUV(float3 direction)
{
	// atan(0, 0) is undefined: straight up or down, where every column shows the same sky, the first one is read.
	float azimuth = direction.x != 0.0 || direction.z != 0.0 ? atan2(direction.z, direction.x) : 0.0;
	float u = frac(azimuth / TWO_PI);
	float elevation = asin(clamp(direction.y, -1.0, 1.0));
	float t = sign(elevation) * sqrt(abs(elevation) / (0.5 * PI));
	float v = UnitToTableUV(float2(0.0, t * 0.5 + 0.5), ST_SKY_VIEW_SIZE).y;
	return float2(u, v);
}

float RayleighPhase(float cosTheta)
{
	return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta);
}

float MiePhase(float cosTheta)
{
	float g = c_MieAsymmetry;
	float denominator = 1.0 + g * g - 2.0 * g * cosTheta;
	return (1.0 - g * g) / (4.0 * PI * denominator * sqrt(denominator));
}

#endif
