#ifndef STRATA_ATMOSPHERE_PASS_GLSL
#define STRATA_ATMOSPHERE_PASS_GLSL

#include "Include/Atmosphere.glsl"

// What the atmosphere's passes (IBL/Atmosphere*.comp) bind: their constants, the tables computed before them (each pass binds
// a black texture where a table is still to be written) and their output.

// RenderData::AtmosphereConstants
layout(set = 0, binding = ST_CBV(0), std140) uniform AtmosphereBuffer
{
	vec4 RayleighScattering; // rgb: per kilometer
	vec4 MieScattering;
	vec4 MieExtinction;
	vec4 OzoneAbsorption;
	vec4 GroundAlbedo;       // rgb
	vec4 SunDirection;       // xyz: toward the sun
	vec4 SunColor;           // rgb: the sun's color over its largest component (the tables hold the sky for 1 lux of it)
	vec4 Viewer;             // x: the viewer's distance from the planet's center (km)
} u_Atmosphere;

// RenderData::AtmospherePassParameters. A shader may have one push-constant block: a pass with its own defines
// ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS before including this file (IBL/AtmosphereAerial.comp).
#ifndef ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS
layout(push_constant) uniform AtmospherePassParameters
{
	uint FirstFace; // IBL/AtmosphereCube.comp: the cube face of the dispatch's first layer
	uint FaceSize;  // ...and the faces' size in texels
	uint Padding0;
	uint Padding1;
} u_Pass;
#endif

layout(set = 0, binding = ST_SRV(0)) uniform texture2D u_TransmittanceTable;
layout(set = 0, binding = ST_SRV(1)) uniform texture2D u_MultipleScatteringTable;
layout(set = 0, binding = ST_SRV(2)) uniform texture2D u_SkyViewTable;
layout(set = 0, binding = ST_SAMPLER(0)) uniform sampler u_TableSampler; // Linear; repeats along u (the sky view's azimuth)

AtmosphereMedium GetMedium()
{
	AtmosphereMedium medium;
	medium.RayleighScattering = u_Atmosphere.RayleighScattering.rgb;
	medium.MieScattering = u_Atmosphere.MieScattering.rgb;
	medium.MieExtinction = u_Atmosphere.MieExtinction.rgb;
	medium.OzoneAbsorption = u_Atmosphere.OzoneAbsorption.rgb;
	return medium;
}

// The sun's transmittance from a point (radius, the sun's cosine to its zenith), zero where the ground hides the sun.
vec3 GetSunTransmittance(float radius, float sunMu)
{
	if (RayMeetsGround(radius, sunMu))
		return vec3(0.0);
	vec2 uv = RadiusMuToTransmittanceUV(radius, sunMu);
	return textureLod(sampler2D(u_TransmittanceTable, u_TableSampler), vec2(clamp(uv.x, 0.0, 1.0), uv.y), 0.0).rgb;
}

// The light of the second and higher scattering orders at a point, per lux of sun (IBL/AtmosphereMultipleScattering.comp).
vec3 GetMultipleScattering(float radius, float sunMu)
{
	return textureLod(sampler2D(u_MultipleScatteringTable, u_TableSampler), RadiusMuToMultipleScatteringUV(radius, sunMu), 0.0).rgb;
}

// One segment of a view ray through the air, from a viewer at `viewerRadius` (km from the center) along `direction` (unit, the
// cosine `mu` with its zenith): the segment `stepLength` km long centered `t` km from the viewer scatters the sun's light toward
// the viewer once (Rayleigh and Mie, of the given phases; the sun's transmittance from the table, zero in the planet's shadow)
// and that of all higher orders (the multiple-scattering table), per lux of sun, dimmed by what lies before it
// (`transmittance`, which the segment's own extinction then multiplies). The sky-view table and aerial perspective integrate
// the same light with it. Points below the ground count as the ground's.
void AccumulateAtmosphereSegment(AtmosphereMedium medium, float viewerRadius, float mu, vec3 direction, vec3 toSun, float rayleighPhase,
	float miePhase, float t, float stepLength, inout vec3 transmittance, inout vec3 light)
{
	float pointRadius = max(RadiusAlongRay(viewerRadius, mu, t), c_AtmosphereBottomRadius);
	float height = pointRadius - c_AtmosphereBottomRadius;
	vec3 rayleigh = medium.RayleighScattering * exp(-max(height, 0.0) / c_RayleighScaleHeight);
	vec3 mie = medium.MieScattering * exp(-max(height, 0.0) / c_MieScaleHeight);
	vec3 extinction = GetAtmosphereExtinction(medium, height);
	vec3 position = vec3(0.0, viewerRadius, 0.0) + direction * t;
	float sunMu = clamp(dot(position, toSun) / pointRadius, -1.0, 1.0);
	vec3 sun = GetSunTransmittance(pointRadius, sunMu);
	vec3 multiple = GetMultipleScattering(pointRadius, sunMu);
	vec3 scattered = rayleigh * (rayleighPhase * sun + multiple) + mie * (miePhase * sun + multiple);
	vec3 stepTransmittance = exp(-extinction * stepLength);
	light += transmittance * (1.0 - stepTransmittance) / max(extinction, vec3(1.0e-9)) * scattered;
	transmittance *= stepTransmittance;
}

#endif
