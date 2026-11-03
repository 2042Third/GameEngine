#ifndef STRATA_ATMOSPHERE_PASS_SLANG
#define STRATA_ATMOSPHERE_PASS_SLANG

#include "Include/Atmosphere.slang"

// What the atmosphere's passes (IBL/Atmosphere*.comp) bind: their constants, the tables computed before them (each pass binds
// a black texture where a table is still to be written) and their output.

// RenderData::AtmosphereConstants
struct AtmosphereBuffer
{
	float4 RayleighScattering; // rgb: per kilometer
	float4 MieScattering;
	float4 MieExtinction;
	float4 OzoneAbsorption;
	float4 GroundAlbedo;       // rgb
	float4 SunDirection;       // xyz: toward the sun
	float4 SunColor;           // rgb: the sun's color over its largest component (the tables hold the sky for 1 lux of it)
	float4 Viewer;             // x: the viewer's distance from the planet's center (km)
};
ConstantBuffer<AtmosphereBuffer> u_Atmosphere : register(b0);

// RenderData::AtmospherePassParameters. A shader may have one push-constant block: a pass with its own defines
// ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS before including this file (IBL/AtmosphereAerial.comp).
#ifndef ST_ATMOSPHERE_CUSTOM_PUSH_CONSTANTS
struct AtmospherePassParameters
{
	uint FirstFace; // IBL/AtmosphereCube.comp: the cube face of the dispatch's first layer
	uint FaceSize;  // ...and the faces' size in texels
	uint Padding0;
	uint Padding1;
};
[[vk::push_constant]] ConstantBuffer<AtmospherePassParameters> u_Pass;
#endif

Texture2D<float4> u_TransmittanceTable : register(t0);
Texture2D<float4> u_MultipleScatteringTable : register(t1);
Texture2D<float4> u_SkyViewTable : register(t2);
SamplerState u_TableSampler : register(s0); // Linear; repeats along u (the sky view's azimuth)

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
float3 GetSunTransmittance(float radius, float sunMu)
{
	if (RayMeetsGround(radius, sunMu))
		return float3(0.0);
	float2 uv = RadiusMuToTransmittanceUV(radius, sunMu);
	return u_TransmittanceTable.SampleLevel(u_TableSampler, float2(clamp(uv.x, 0.0, 1.0), uv.y), 0.0).rgb;
}

// The light of the second and higher scattering orders at a point, per lux of sun (IBL/AtmosphereMultipleScattering.comp, which
// stores it scaled by ST_ATMOSPHERE_STORAGE_SCALE).
float3 GetMultipleScattering(float radius, float sunMu)
{
	return u_MultipleScatteringTable.SampleLevel(u_TableSampler, RadiusMuToMultipleScatteringUV(radius, sunMu), 0.0).rgb * (1.0 / ST_ATMOSPHERE_STORAGE_SCALE);
}

// One segment of a view ray through the air, from a viewer at `viewerRadius` (km from the center) along `direction` (unit, the
// cosine `mu` with its zenith): the segment `stepLength` km long centered `t` km from the viewer scatters the sun's light toward
// the viewer once (Rayleigh and Mie, of the given phases; the sun's transmittance from the table, zero in the planet's shadow)
// and that of all higher orders (the multiple-scattering table), per lux of sun, dimmed by what lies before it
// (`transmittance`, which the segment's own extinction then multiplies). The sky-view table and aerial perspective integrate
// the same light with it. Points below the ground count as the ground's.
void AccumulateAtmosphereSegment(AtmosphereMedium medium, float viewerRadius, float mu, float3 direction, float3 toSun, float rayleighPhase,
	float miePhase, float t, float stepLength, inout float3 transmittance, inout float3 light)
{
	float pointRadius = max(RadiusAlongRay(viewerRadius, mu, t), c_AtmosphereBottomRadius);
	float height = pointRadius - c_AtmosphereBottomRadius;
	float3 rayleigh = medium.RayleighScattering * exp(-max(height, 0.0) / c_RayleighScaleHeight);
	float3 mie = medium.MieScattering * exp(-max(height, 0.0) / c_MieScaleHeight);
	float3 extinction = GetAtmosphereExtinction(medium, height);
	float3 position = float3(0.0, viewerRadius, 0.0) + direction * t;
	float sunMu = clamp(dot(position, toSun) / pointRadius, -1.0, 1.0);
	float3 sun = GetSunTransmittance(pointRadius, sunMu);
	float3 multiple = GetMultipleScattering(pointRadius, sunMu);
	float3 scattered = rayleigh * (rayleighPhase * sun + multiple) + mie * (miePhase * sun + multiple);
	float3 stepTransmittance = exp(-extinction * stepLength);
	light += transmittance * (1.0 - stepTransmittance) / max(extinction, float3(1.0e-9)) * scattered;
	transmittance *= stepTransmittance;
}

#endif
