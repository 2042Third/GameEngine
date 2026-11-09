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
Texture3D<uint> u_GIFieldDepth : register(t54); // Two halves a texel (PackHalf2x16): the mean and the variance

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

// Whether a point lies before every ray of a cascade-0 probe around its direction hit anything (FieldReduce.comp's depth moments):
// nearer than the nearest of the four texels of the probe's tile around the direction's octahedral point, each texel's mean
// distance less twice its spread (meters). There the guard has nothing to find.
bool LiesBeforeProbeHits(int3 probe, float3 probePosition, float3 target)
{
	float3 toTarget = target - probePosition;
	float distance = length(toTarget);
	if (distance <= 1e-5)
		return true;
	float interior = float(ST_GI_FIELD_DEPTH_TEXELS - 2);
	// Texel k of the tile spans [k, k + 1); the interior starts at 1, and the four texels are those around the point.
	float2 coordinate = (EncodeOctahedral(toTarget / distance) * 0.5 + 0.5) * interior + 0.5;
	int2 first = clamp(int2(floor(coordinate)), int2(0, 0), int2(ST_GI_FIELD_DEPTH_TEXELS - 2, ST_GI_FIELD_DEPTH_TEXELS - 2));
	int2 origin = probe.xy * ST_GI_FIELD_DEPTH_TEXELS + first;
	float2 a = UnpackHalf2x16(u_GIFieldDepth.Load(int4(origin, probe.z, 0)));
	float2 b = UnpackHalf2x16(u_GIFieldDepth.Load(int4(origin + int2(1, 0), probe.z, 0)));
	float2 c = UnpackHalf2x16(u_GIFieldDepth.Load(int4(origin + int2(0, 1), probe.z, 0)));
	float2 d = UnpackHalf2x16(u_GIFieldDepth.Load(int4(origin + int2(1, 1), probe.z, 0)));
	float4 nearest = float4(a.x, b.x, c.x, d.x) - 2.0 * sqrt(float4(a.y, b.y, c.y, d.y));
	return distance <= min(min(nearest.x, nearest.y), min(nearest.z, nearest.w));
}

// The point the gather's probes are asked about, moved out of a solid it lies in: within about a voxel of a perpendicular wall,
// floor or object (a concave corner, a contact) the biased point lies in that surface's voxels, where every probe's guard and
// grazing distances would hide it. Down the occupancy's gradient, ST_GI_GATHER_PUSH_STEPS times at most a voxel of the finest
// level holding it, each by how far its occupancy lies into ST_GI_GATHER_PUSH_OCCUPANCY (continuous: no step where a point starts
// to move). The gradient leads out on the side the point is nearer, so it never crosses a wall thicker than two voxels.
float3 PushOutOfSolid(SamplerState repeat, float3 position)
{
	uint level = FindFieldLevel(position, position);
	if (level >= 3u)
		return position;
	float h = u_GIField.VoxelOrigin[level].w;
	[loop]
	for (uint step = 0u; step < ST_GI_GATHER_PUSH_STEPS; step++)
	{
		float push = smoothstep(ST_GI_GATHER_PUSH_OCCUPANCY.x, ST_GI_GATHER_PUSH_OCCUPANCY.y, SampleFieldOccupancy(level, repeat, position));
		if (push <= 0.0)
			break;
		float3 gradient = float3(
			SampleFieldOccupancy(level, repeat, position + float3(h, 0.0, 0.0)) - SampleFieldOccupancy(level, repeat, position - float3(h, 0.0, 0.0)),
			SampleFieldOccupancy(level, repeat, position + float3(0.0, h, 0.0)) - SampleFieldOccupancy(level, repeat, position - float3(0.0, h, 0.0)),
			SampleFieldOccupancy(level, repeat, position + float3(0.0, 0.0, h)) - SampleFieldOccupancy(level, repeat, position - float3(0.0, 0.0, h)));
		float gradientLength = length(gradient);
		if (gradientLength < 1e-4)
			break;
		position -= gradient / gradientLength * (push * h);
	}
	return position;
}

// The field's irradiance over pi at a camera-relative surface point for a shading normal (luminance, times
// ST_GI_RADIANCE_SCALE), .a the field's weight there (0 outside it, while pending or where no voxel level holds the point). The
// eight cascade-0 probes around the point moved ST_GI_GATHER_NORMAL_BIAS spacings along its geometric normal, each weighted
// trilinearly, by its validity, by whether it lies in front of the surface's plane and by whether the voxels between it and the
// point let it see the point (GetFieldTransmittance, toward the moved point moved ST_GI_GATHER_VIEW_BIAS spacings more toward the
// viewer and out of any solid it then lies in when pushOut, PushOutOfSolid: neither a surface's own voxels nor a perpendicular
// one's beside it hide it; skipped where the point lies before every ray of the probe around it hit anything), then normalized.
// The guard is the geometry itself: Chebyshev's bound on the probes' 64 depth directions blurred a contact's surfaces over 25
// degrees and hid probes that see the point plainly (a floor point under the Cornell box's sphere, beside its tall box), and the
// light there went black. Where the live probes in front of the point see less of it than ST_GI_GATHER_MIN_VISIBLE of the weight
// they would give it (a junction, a wall between), the light fades with what they see rather than giving the few that see it
// full weight; dead probes take no part (a probe inside an object over a contact says nothing about the floor beside it); a
// point no probe sees takes no field light (dark, never the sky). Every weight is continuous, so the light is: no bands at cell
// borders, no staircase of voxels. openShare receives the cosine-weighted share of the sky it sees. viewDirection points toward
// the viewer (the normal where there is none). pushOut moves the visibility point out of solids: drawn surfaces only.
// Re-injection reads the field at voxels, some of them inside walls (the floor's face runs on under a wall), and moved out of the
// solid such a point would take the light on the wall's far side into a sealed room.
float4 SampleIrradianceField(SamplerState repeat, float3 position, float3 geometricNormal, float3 shadingNormal, float3 viewDirection,
	bool pushOut, out float openShare)
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
	if (pushOut)
		seen = PushOutOfSolid(repeat, seen);

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
		float weight = trilinear3.x * trilinear3.y * trilinear3.z * facing * validity;
		reachable += weight;
		if (weight <= 0.0)
			continue;
		// The guard marches only where the probe's rays around the point hit something before it.
		if (!LiesBeforeProbeHits(probe, probePosition, seen))
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
