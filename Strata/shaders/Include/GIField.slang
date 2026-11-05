#ifndef STRATA_GI_FIELD_SLANG
#define STRATA_GI_FIELD_SLANG

// Global illumination's irradiance field as the scene passes read it (Docs/DeveloperGuide.md, "Global illumination"): b3 of the
// scene layout (RenderData::GIFieldConstants, zero without GI), the field's faces (SRV 35), cascade 0's validity (SRV 37), its
// probes' depth moments (SRV 54) and voxel levels 0-2 (SRV 38-40) for the gather's visibility. Everything is world data: no screen
// input.

#include "Include/Common.slang"

#include "Include/GlobalIllumination.slang"

struct GIFieldConstants
{
	float4 FieldOrigin;          // xyz: the field's minimum corner, camera-relative (cascade 0's probe i at + (i + 0.5) spacings); w: s0
	float4 FieldSize;            // xyz: cascade 0's probes per axis; w: 1 when the field is used this frame (ran, not pending)
	float4 FieldFade;            // xyz: the half extents around the camera where the field's weight reaches 0 (m); w: the fade (m)
	float4 VoxelOrigin[3];       // Levels 0-2: xyz the window's minimum corner, camera-relative (m); w the voxel size, 0 when not valid
	int4 VoxelTexel[3];          // xyz: the minimum corner's texel
	int4 VoxelValidMinimum[3];   // xyz: the valid box's first voxel, relative to the window's minimum corner
	int4 VoxelValidMaximum[3];   // xyz: one past its last voxel
	float4 VoxelDimensions;      // xyz: voxels per axis
};
ConstantBuffer<GIFieldConstants> u_GIField : register(b3);

Texture3D<float4> u_GIFieldFaces : register(t35);
Texture3D<float4> u_GIFieldValidity : register(t37);
Texture3D<float4> u_GIFieldVoxels0 : register(t38);
Texture3D<float4> u_GIFieldVoxels1 : register(t39);
Texture3D<float4> u_GIFieldVoxels2 : register(t40);
Texture3D<float4> u_GIFieldDepth : register(t54);

// The field's weight at a camera-relative point: 1 within its full extent around the camera, falling to 0 over the fade (by the
// distance from the camera, not from the snapped field, so it moves continuously), times FieldSize.w (0 while pending).
float GetFieldWeight(float3 position)
{
	float3 margin = u_GIField.FieldFade.xyz - abs(position);
	return u_GIField.FieldSize.w * saturate(min(margin.x, min(margin.y, margin.z)) / max(u_GIField.FieldFade.w, 1e-4));
}

// A cascade-0 probe's ambient cube read along a normal: n.x^2 E(+-X) + n.y^2 E(+-Y) + n.z^2 E(+-Z), the faces picked by the
// normal's signs (rgb irradiance over pi times ST_GI_RADIANCE_SCALE, a the sky share).
float4 ReadAmbientCube(int3 probe, float3 normal)
{
	int3 base = int3(probe.x * 3, probe.y * 2, probe.z);
	float3 squared = normal * normal;
	float4 x = u_GIFieldFaces.Load(int4(base + (normal.x >= 0.0 ? int3(0, 0, 0) : int3(1, 0, 0)), 0));
	float4 y = u_GIFieldFaces.Load(int4(base + (normal.y >= 0.0 ? int3(2, 0, 0) : int3(0, 1, 0)), 0));
	float4 z = u_GIFieldFaces.Load(int4(base + (normal.z >= 0.0 ? int3(1, 1, 0) : int3(2, 1, 0)), 0));
	return squared.x * x + squared.y * y + squared.z * z;
}

float SampleFieldVoxels(uint level, SamplerState repeat, float3 coordinate)
{
	if (level == 0u)
		return u_GIFieldVoxels0.SampleLevel(repeat, coordinate, 0.0).a;
	if (level == 1u)
		return u_GIFieldVoxels1.SampleLevel(repeat, coordinate, 0.0).a;
	return u_GIFieldVoxels2.SampleLevel(repeat, coordinate, 0.0).a;
}

// Whether a camera-relative point lies in a level's valid box, less ST_GI_LEVEL_GUARD_VOXELS (trilinear filtering reads beyond).
bool IsInFieldLevel(uint level, float3 position)
{
	float voxel = u_GIField.VoxelOrigin[level].w;
	if (voxel <= 0.0)
		return false;
	float3 local = (position - u_GIField.VoxelOrigin[level].xyz) / voxel;
	return all(local >= float3(u_GIField.VoxelValidMinimum[level].xyz) + ST_GI_LEVEL_GUARD_VOXELS)
		&& all(local <= float3(u_GIField.VoxelValidMaximum[level].xyz) - ST_GI_LEVEL_GUARD_VOXELS);
}

// The finest of levels 0-2 whose valid box holds both points (3: none).
uint FindFieldLevel(float3 a, float3 b)
{
	[loop]
	for (uint level = 0u; level < 3u; level++)
	{
		if (IsInFieldLevel(level, a) && IsInFieldLevel(level, b))
			return level;
	}
	return 3u;
}

float SampleFieldOccupancy(uint level, SamplerState repeat, float3 position)
{
	float voxel = u_GIField.VoxelOrigin[level].w;
	float3 coordinate = ((position - u_GIField.VoxelOrigin[level].xyz) / voxel + float3(u_GIField.VoxelTexel[level].xyz)) / u_GIField.VoxelDimensions.xyz;
	return SampleFieldVoxels(level, repeat, coordinate);
}

// The transmittance through the voxels from a probe to the point it lights (the gather's guard against walls):
// ST_GI_GATHER_GUARD_STEPS samples at fixed fractions of the segment, under a voxel apart over a cell's diagonal at either
// quality (a fixed count, so it changes continuously with the point), through the finest of levels 0-2 whose valid box holds
// both ends; none holds them: 0 (nothing tells whether a wall lies between, and dark is never a leak). Chebyshev's bound on a
// probe's 64 directions lets a thin wall seen at a grazing angle through; a solid voxel on the segment does not.
float GetFieldTransmittance(SamplerState repeat, float3 from, float3 to)
{
	uint level = FindFieldLevel(from, to);
	if (level >= 3u)
		return 0.0;
	float transmittance = 1.0;
	[loop]
	for (uint step = 0u; step < ST_GI_GATHER_GUARD_STEPS; step++)
	{
		float3 position = lerp(from, to, (float(step) + 0.5) / float(ST_GI_GATHER_GUARD_STEPS));
		transmittance *= 1.0 - smoothstep(ST_GI_GATHER_GUARD_CORE.x, ST_GI_GATHER_GUARD_CORE.y, SampleFieldOccupancy(level, repeat, position));
	}
	return transmittance;
}

// A cascade-0 probe's distance to the first hit toward a direction (FieldReduce.comp's depth moments): the four texels of its tile
// around the direction's octahedral point, bilinear, as the mixture of their distributions: x the mean distance, y its variance
// (meters); z the nearest of the four texels' means less twice their spreads (a point nearer lies before every ray around it
// hit anything).
float3 ReadProbeDepth(int3 probe, float3 direction)
{
	float interior = float(ST_GI_FIELD_DEPTH_TEXELS - 2);
	// Texel k of the tile spans [k, k + 1); the interior starts at 1, and bilinear reads are between texel centers.
	float2 coordinate = (EncodeOctahedral(direction) * 0.5 + 0.5) * interior + 0.5;
	int2 first = clamp(int2(floor(coordinate)), int2(0, 0), int2(ST_GI_FIELD_DEPTH_TEXELS - 2, ST_GI_FIELD_DEPTH_TEXELS - 2));
	float2 fraction = saturate(coordinate - float2(first));
	int2 origin = probe.xy * ST_GI_FIELD_DEPTH_TEXELS + first;
	float2 a = u_GIFieldDepth.Load(int4(origin, probe.z, 0)).rg;
	float2 b = u_GIFieldDepth.Load(int4(origin + int2(1, 0), probe.z, 0)).rg;
	float2 c = u_GIFieldDepth.Load(int4(origin + int2(0, 1), probe.z, 0)).rg;
	float2 d = u_GIFieldDepth.Load(int4(origin + int2(1, 1), probe.z, 0)).rg;
	float4 weights = float4((1.0 - fraction.x) * (1.0 - fraction.y), fraction.x * (1.0 - fraction.y), (1.0 - fraction.x) * fraction.y,
		fraction.x * fraction.y);
	float mean = dot(weights, float4(a.x, b.x, c.x, d.x));
	float4 spread = float4(a.x, b.x, c.x, d.x) - mean;
	float variance = dot(weights, float4(a.y, b.y, c.y, d.y) + spread * spread);
	float4 near = float4(a.x, b.x, c.x, d.x) - 2.0 * sqrt(float4(a.y, b.y, c.y, d.y));
	return float3(mean, variance, min(min(near.x, near.y), min(near.z, near.w)));
}

// How much a probe sees a point (Chebyshev's bound on the probe's distances toward it, the test of DDGI, Majercik et al. 2019;
// ideas only): 1 where the point lies no farther than the mean distance to the first hit; beyond it the share of the distribution
// the bound leaves, cubed so the light falls off within the hit's spread rather than over its tail. Continuous: no step where a
// probe stops seeing a point. `clear` receives whether the point lies before every ray around it hit anything (the guard has
// nothing to find there).
float GetProbeVisibility(int3 probe, float3 probePosition, float3 target, out bool clear)
{
	float3 toTarget = target - probePosition;
	float distance = length(toTarget);
	clear = distance <= 1e-5;
	if (clear)
		return 1.0;
	float3 depth = ReadProbeDepth(probe, toTarget / distance);
	clear = distance <= depth.z;
	if (distance <= depth.x)
		return 1.0;
	float excess = distance - depth.x;
	float chebyshev = depth.y / max(depth.y + excess * excess, 1e-12);
	return chebyshev * chebyshev * chebyshev;
}

// The field's irradiance over pi at a camera-relative surface point for a shading normal (luminance, times
// ST_GI_RADIANCE_SCALE), .a the field's weight there (0 outside it, while pending or where no voxel level holds the point). The
// eight cascade-0 probes around the point moved ST_GI_GATHER_NORMAL_BIAS spacings along its geometric normal, each weighted
// trilinearly, by its validity, by whether it lies in front of the surface's plane and by how much it sees the point
// (GetProbeVisibility, at the moved point moved ST_GI_GATHER_VIEW_BIAS spacings more toward the viewer: a surface's own voxels
// never hide it; times the transmittance of the voxels between them, GetFieldTransmittance), then normalized. Where the live
// probes in front of the point see less of it than ST_GI_GATHER_MIN_VISIBLE of the weight all the probes in front would give it
// (a contact, a junction, a cell of probes dead inside a solid), the light fades with what they see rather than giving the few
// that see it full weight; a point no probe sees takes no field light (dark, never the sky). Every weight is continuous, so the
// light is: no bands at cell borders, no staircase of voxels. openShare receives the cosine-weighted share of the sky it sees.
// viewDirection points toward the viewer (the normal where there is none).
float4 SampleIrradianceField(SamplerState repeat, float3 position, float3 geometricNormal, float3 shadingNormal, float3 viewDirection,
	out float openShare)
{
	openShare = 1.0;
	float fieldWeight = GetFieldWeight(position);
	// A point no voxel level holds is outside what the probes' marches saw: today's light, as outside the field. The field's
	// reach stays where level 2 holds every point one scroll slab short (GlobalIllumination::ComputeLayout), so only a camera that
	// moved on by more than a step before the slab was voxelized meets this.
	if (fieldWeight <= 0.0 || FindFieldLevel(position, position) >= 3u)
		return float4(0.0, 0.0, 0.0, 0.0);
	float spacing = u_GIField.FieldOrigin.w;
	int3 grid = int3(u_GIField.FieldSize.xyz);
	float3 biased = position + geometricNormal * (ST_GI_GATHER_NORMAL_BIAS * spacing);
	float3 coordinate = (biased - u_GIField.FieldOrigin.xyz) / spacing - 0.5;
	int3 base = int3(floor(coordinate));
	float3 fraction = coordinate - float3(base);
	float3 seen = biased + viewDirection * (ST_GI_GATHER_VIEW_BIAS * spacing);

	float3 irradiance = float3(0.0, 0.0, 0.0);
	float sky = 0.0;
	float total = 0.0;
	float reachable = 0.0;
	[loop]
	for (uint corner = 0u; corner < 8u; corner++)
	{
		int3 offset = int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		int3 probe = base + offset;
		if (any(probe < int3(0, 0, 0)) || any(probe >= grid))
			continue;
		float4 validity4 = u_GIFieldValidity.Load(int4(probe, 0));
		float validity = validity4.r;
		// Where the probe's light and distances were measured: moved off a surface it sat on (ProbeValidity.comp).
		float3 probePosition = u_GIField.FieldOrigin.xyz + (float3(probe) + 0.5) * spacing + DecodeProbeMove(validity4, spacing);
		float3 toProbe = probePosition - position;
		float facing = smoothstep(-0.05, 0.25, dot(toProbe * rsqrt(max(dot(toProbe, toProbe), 1e-8)), geometricNormal));
		float3 trilinear3 = select(offset != int3(0, 0, 0), fraction, 1.0 - fraction);
		float weight = trilinear3.x * trilinear3.y * trilinear3.z * facing;
		reachable += weight;
		weight *= validity;
		if (weight <= 0.0)
			continue;
		bool clear;
		weight *= GetProbeVisibility(probe, probePosition, seen, clear);
		if (weight <= 0.0)
			continue;
		// The guard marches only where the probe's rays around the point hit something before it.
		if (!clear)
			weight *= GetFieldTransmittance(repeat, probePosition, seen);
		if (weight <= 0.0)
			continue;
		float4 cube = ReadAmbientCube(probe, shadingNormal);
		irradiance += weight * cube.rgb;
		sky += weight * cube.a;
		total += weight;
	}
	openShare = total > 0.0 ? sky / total : 0.0;
	float normalization = max(total, ST_GI_GATHER_MIN_VISIBLE * reachable);
	return float4(total > 0.0 ? irradiance / normalization : float3(0.0, 0.0, 0.0), fieldWeight);
}

#endif
