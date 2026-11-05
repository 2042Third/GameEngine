#ifndef STRATA_GI_PROBES_SLANG
#define STRATA_GI_PROBES_SLANG

// What global illumination's light passes share (GI/VoxelInject.comp, ProbeValidity.comp, ProbeWeights.comp, CascadeMarch.comp,
// FieldReduce.comp; Docs/DeveloperGuide.md, "Global illumination"): the voxel levels' textures, the probe passes' push
// constants, and SampleVoxels, the march's sample of the clipmap. Layouts: SceneRendererGlobalIllumination.cpp's light passes
// (b0 the frame's constants, b1 the GI's, b2 these push constants; SRV 6 the environment cube, sampler 8 linear clamp).

#include "Include/Common.slang"

#include "Include/GlobalIllumination.slang"
#include "Include/Scene.slang"

Texture3D<float4> u_GIMaterial0 : register(t41);
Texture3D<float4> u_GIMaterial1 : register(t42);
Texture3D<float4> u_GIMaterial2 : register(t43);
Texture3D<float4> u_GIMaterial3 : register(t44);
Texture3D<float4> u_GINormal0 : register(t45);
Texture3D<float4> u_GINormal1 : register(t46);
Texture3D<float4> u_GINormal2 : register(t47);
Texture3D<float4> u_GINormal3 : register(t48);
Texture3D<float4> u_GIRadiance0 : register(t49);
Texture3D<float4> u_GIRadiance1 : register(t50);
Texture3D<float4> u_GIRadiance2 : register(t51);
Texture3D<float4> u_GIRadiance3 : register(t52);
// Trilinear filtering of the toroidal levels (linear filtering, repeating addresses).
SamplerState u_GIRepeat : register(s12);

// RenderData::GIProbePassParameters.
struct GIProbePassParameters
{
	uint4 Grid;          // xyz: the cascade's probes per axis; w: the cascade
	uint4 ParentGrid;    // xyz: the next cascade's probes per axis (0 for the top one); w: its directions per axis
	float4 Interval;     // x: spacing (m); y: the interval's start, z: its end plus its overlap (negative for the top: until the ray leaves); w: lead-in
	uint DirectionsPerAxis;
	uint MinimumLevel;
	uint Level;
	uint SubRays;        // CascadeMarch: each texel is the mean of SubRays x SubRays rays spread over it
};
[[vk::push_constant]] ConstantBuffer<GIProbePassParameters> u_Pass;

float4 SampleGIMaterial(uint level, float3 coordinate)
{
	if (level == 0u)
		return u_GIMaterial0.SampleLevel(u_GIRepeat, coordinate, 0.0);
	if (level == 1u)
		return u_GIMaterial1.SampleLevel(u_GIRepeat, coordinate, 0.0);
	if (level == 2u)
		return u_GIMaterial2.SampleLevel(u_GIRepeat, coordinate, 0.0);
	return u_GIMaterial3.SampleLevel(u_GIRepeat, coordinate, 0.0);
}

float4 SampleGINormal(uint level, float3 coordinate)
{
	if (level == 0u)
		return u_GINormal0.SampleLevel(u_GIRepeat, coordinate, 0.0);
	if (level == 1u)
		return u_GINormal1.SampleLevel(u_GIRepeat, coordinate, 0.0);
	if (level == 2u)
		return u_GINormal2.SampleLevel(u_GIRepeat, coordinate, 0.0);
	return u_GINormal3.SampleLevel(u_GIRepeat, coordinate, 0.0);
}

float4 SampleGIRadiance(uint level, float3 coordinate)
{
	if (level == 0u)
		return u_GIRadiance0.SampleLevel(u_GIRepeat, coordinate, 0.0);
	if (level == 1u)
		return u_GIRadiance1.SampleLevel(u_GIRepeat, coordinate, 0.0);
	if (level == 2u)
		return u_GIRadiance2.SampleLevel(u_GIRepeat, coordinate, 0.0);
	return u_GIRadiance3.SampleLevel(u_GIRepeat, coordinate, 0.0);
}

float4 LoadGIMaterial(uint level, int3 texel)
{
	if (level == 0u)
		return u_GIMaterial0.Load(int4(texel, 0));
	if (level == 1u)
		return u_GIMaterial1.Load(int4(texel, 0));
	if (level == 2u)
		return u_GIMaterial2.Load(int4(texel, 0));
	return u_GIMaterial3.Load(int4(texel, 0));
}

float4 LoadGINormal(uint level, int3 texel)
{
	if (level == 0u)
		return u_GINormal0.Load(int4(texel, 0));
	if (level == 1u)
		return u_GINormal1.Load(int4(texel, 0));
	if (level == 2u)
		return u_GINormal2.Load(int4(texel, 0));
	return u_GINormal3.Load(int4(texel, 0));
}

// The clipmap's occupancy at a camera-relative point, blended over the levels from `minimumLevel` up that hold it
// (GetLevelWeight), and the finest of them (ST_GI_MAX_VOXEL_LEVELS: no level holds it, the point left the clipmap).
float SampleOccupancy(float3 position, uint minimumLevel, out uint finest)
{
	float occupancy = 0.0;
	float remaining = 1.0;
	finest = ST_GI_MAX_VOXEL_LEVELS;
	for (uint level = minimumLevel; level < u_GI.Dimensions.w && remaining > 0.0; level++)
	{
		float weight = GetLevelWeight(level, position);
		if (weight <= 0.0)
			continue;
		if (finest == ST_GI_MAX_VOXEL_LEVELS)
			finest = level;
		occupancy += remaining * weight * SampleGIMaterial(level, GetVoxelTextureCoordinate(level, position)).a;
		remaining *= 1.0 - weight;
	}
	return occupancy;
}

// The light leaving the surface voxels around a camera-relative point toward `direction` (scaled luminance), blended as the
// occupancy is: each level's radiance is stored times its surface weight (alpha), so rgb / a is the surfaces' radiance alone. A
// surface faces the ray by its normal (back faces send nothing; double-sided voxels send to both sides; LAB's facing weight).
float3 SampleSurfaceRadiance(float3 position, uint minimumLevel, float3 direction)
{
	float4 radiance = float4(0.0, 0.0, 0.0, 0.0);
	float4 normal = float4(0.0, 0.0, 0.0, 0.0);
	float remaining = 1.0;
	for (uint level = minimumLevel; level < u_GI.Dimensions.w && remaining > 0.0; level++)
	{
		float weight = GetLevelWeight(level, position);
		if (weight <= 0.0)
			continue;
		float3 coordinate = GetVoxelTextureCoordinate(level, position);
		radiance += remaining * weight * SampleGIRadiance(level, coordinate);
		normal += remaining * weight * SampleGINormal(level, coordinate);
		remaining *= 1.0 - weight;
	}
	if (radiance.a <= 1e-6)
		return float3(0.0, 0.0, 0.0);
	float normalLength = length(normal.xyz);
	float facing = normalLength > 1e-4 ? smoothstep(-0.15, 0.25, dot(-direction, normal.xyz / normalLength)) : 1.0;
	// Double-sided voxels (w 0.5) light both sides; one-sided (w 1) and two-sided (w -1, dark) ones keep the facing.
	float doubleSided = saturate(1.0 - abs(normal.w / max(radiance.a, 1e-4) - 0.5) * 4.0);
	return radiance.rgb / radiance.a * lerp(facing, 1.0, doubleSided);
}

// The surfaces' normal around a camera-relative point, blended as the occupancy is (unnormalized: zero where no surface faces
// it, inside solids and between two-sided faces).
float3 SampleSurfaceNormal(float3 position, uint minimumLevel)
{
	float3 normal = float3(0.0, 0.0, 0.0);
	float remaining = 1.0;
	for (uint level = minimumLevel; level < u_GI.Dimensions.w && remaining > 0.0; level++)
	{
		float weight = GetLevelWeight(level, position);
		if (weight <= 0.0)
			continue;
		normal += remaining * weight * SampleGINormal(level, GetVoxelTextureCoordinate(level, position)).xyz;
		remaining *= 1.0 - weight;
	}
	return normal;
}

// A probe's position, camera-relative: the field's minimum corner plus (index + 0.5) spacings.
float3 GetProbePosition(uint3 index, float spacing)
{
	return u_GI.FieldOrigin.xyz + (float3(index) + 0.5) * spacing;
}

// What the sky sends along a direction a ray leaves the clipmap by (scaled luminance): the environment cube's level about a
// top-cascade direction's size times its intensity, or the constant ambient without an environment.
float3 GetSkyRadiance(float3 direction)
{
	float3 sky = u_Frame.EnvironmentParams.w > 0.0
		? u_EnvironmentMap.SampleLevel(u_LinearClampSampler, RotateEnvironmentDirection(direction), ST_GI_SKY_LEVEL).rgb * u_Frame.EnvironmentParams.x
		: u_Frame.AmbientColor.rgb;
	return min(SanitizeHDR(sky) * ST_GI_RADIANCE_SCALE, float3(ST_GI_MAX_RADIANCE, ST_GI_MAX_RADIANCE, ST_GI_MAX_RADIANCE));
}

#endif
