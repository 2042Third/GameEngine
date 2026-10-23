#ifndef STRATA_ATMOSPHERE_GLSL
#define STRATA_ATMOSPHERE_GLSL

#include "Include/Common.glsl"

// The physical atmosphere of SkyLightSource::Atmosphere (SceneRenderer, the IBL/Atmosphere*.comp passes). Keep the constants,
// the transmittance's quadrature and the table mappings in sync with Renderer/Atmosphere.h: the CPU evaluates the same
// transmittance for the sun's light at the ground. A planet whose ground is the world's y = 0 plane; lengths in kilometers,
// coefficients per kilometer.

const float c_AtmosphereBottomRadius = 6360.0;
const float c_AtmosphereTopRadius = 6460.0;
const float c_RayleighScaleHeight = 8.0;
const float c_MieScaleHeight = 1.2;
const float c_OzoneCenterHeight = 25.0;
const float c_OzoneHalfWidth = 15.0;
const float c_MieAsymmetry = 0.8;
const uint c_TransmittanceSteps = 40u;

#define ST_TRANSMITTANCE_SIZE         vec2(256.0, 64.0)
#define ST_MULTIPLE_SCATTERING_SIZE   vec2(32.0, 32.0)
#define ST_SKY_VIEW_SIZE              vec2(192.0, 108.0)

// The medium's coefficients (RenderData::AtmosphereConstants, scaled by the sky light's density and haze).
struct AtmosphereMedium
{
	vec3 RayleighScattering;
	vec3 MieScattering;
	vec3 MieExtinction;
	vec3 OzoneAbsorption;
};

vec3 GetAtmosphereExtinction(AtmosphereMedium medium, float height)
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
vec3 ComputeTransmittance(AtmosphereMedium medium, float radius, float mu)
{
	float step = DistanceToAtmosphereTop(radius, mu) / float(c_TransmittanceSteps);
	vec3 depth = vec3(0.0);
	for (uint index = 0u; index < c_TransmittanceSteps; index++)
	{
		float t = (float(index) + 0.5) * step;
		depth += GetAtmosphereExtinction(medium, RadiusAlongRay(radius, mu, t) - c_AtmosphereBottomRadius);
	}
	return exp(-depth * step);
}

// Texel centers span each table's whole range: the first and last ones sit on its ends.
vec2 TableUVToUnit(vec2 uv, vec2 size)
{
	return clamp((uv - 0.5 / size) / (1.0 - 1.0 / size), 0.0, 1.0);
}

vec2 UnitToTableUV(vec2 unit, vec2 size)
{
	return unit * (1.0 - 1.0 / size) + 0.5 / size;
}

// The transmittance table: directions crowd toward the horizon (mu = t |t|), heights toward the ground (Atmosphere::
// GetTransmittanceRadiusMu).
vec2 TransmittanceUVToRadiusMu(vec2 uv)
{
	vec2 unit = TableUVToUnit(uv, ST_TRANSMITTANCE_SIZE);
	float t = 2.0 * unit.x - 1.0;
	return vec2(c_AtmosphereBottomRadius + unit.y * unit.y * (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), t * abs(t));
}

vec2 RadiusMuToTransmittanceUV(float radius, float mu)
{
	float height = clamp((radius - c_AtmosphereBottomRadius) / (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), 0.0, 1.0);
	vec2 unit = vec2(0.5 + 0.5 * sign(mu) * sqrt(abs(clamp(mu, -1.0, 1.0))), sqrt(height));
	return UnitToTableUV(unit, ST_TRANSMITTANCE_SIZE);
}

// The multiple-scattering table: by the sun's cosine to the zenith and the height.
vec2 MultipleScatteringUVToRadiusMu(vec2 uv)
{
	vec2 unit = TableUVToUnit(uv, ST_MULTIPLE_SCATTERING_SIZE);
	return vec2(c_AtmosphereBottomRadius + unit.y * (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), unit.x * 2.0 - 1.0);
}

vec2 RadiusMuToMultipleScatteringUV(float radius, float mu)
{
	float height = clamp((radius - c_AtmosphereBottomRadius) / (c_AtmosphereTopRadius - c_AtmosphereBottomRadius), 0.0, 1.0);
	return UnitToTableUV(vec2(clamp(mu, -1.0, 1.0) * 0.5 + 0.5, height), ST_MULTIPLE_SCATTERING_SIZE);
}

// The sky-view table: around the world's vertical axis (u, from +X toward +Z) and by elevation above the viewer's horizontal
// plane (v from straight down to straight up), its rows crowding toward the horizon (elevation = t |t| pi / 2). u wraps.
vec3 SkyViewUVToDirection(vec2 uv)
{
	float azimuth = uv.x * TWO_PI;
	float t = 2.0 * TableUVToUnit(uv, ST_SKY_VIEW_SIZE).y - 1.0;
	float elevation = t * abs(t) * (0.5 * PI);
	float horizontal = cos(elevation);
	return vec3(cos(azimuth) * horizontal, sin(elevation), sin(azimuth) * horizontal);
}

vec2 DirectionToSkyViewUV(vec3 direction)
{
	float azimuth = atan(direction.z, direction.x);
	float u = fract(azimuth / TWO_PI);
	float elevation = asin(clamp(direction.y, -1.0, 1.0));
	float t = sign(elevation) * sqrt(abs(elevation) / (0.5 * PI));
	float v = UnitToTableUV(vec2(0.0, t * 0.5 + 0.5), ST_SKY_VIEW_SIZE).y;
	return vec2(u, v);
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
