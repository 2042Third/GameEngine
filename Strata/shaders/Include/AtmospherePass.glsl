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

// RenderData::AtmospherePassParameters
layout(push_constant) uniform AtmospherePassParameters
{
	uint FirstFace; // IBL/AtmosphereCube.comp: the cube face of the dispatch's first layer
	uint FaceSize;  // ...and the faces' size in texels
	uint Padding0;
	uint Padding1;
} u_Pass;

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

#endif
