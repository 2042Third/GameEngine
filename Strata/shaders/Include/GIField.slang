#ifndef STRATA_GI_FIELD_SLANG
#define STRATA_GI_FIELD_SLANG

// Global illumination's irradiance field as the scene passes read it (Docs/DeveloperGuide.md, "Global illumination"): b3 of the
// scene layout (RenderData::GIFieldConstants, zero without GI), the field's faces (SRV 35), cascade 0's validity (SRV 37) and voxel
// levels 0-2 (SRV 38-40) for the gather's visibility. Everything is world data: no screen input.

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

// The transmittance from a start point to a probe through one level (FindFieldLevel's), one voxel a step; below
// ST_GI_VISIBILITY_CUTOFF nothing is seen. Points no level holds both of are not seen: nothing tells whether a floor or a
// ceiling lies between them (a valid box falls up to two scroll steps short while a scroll is voxelized), and dark is never a
// leak.
float GetFieldVisibility(SamplerState repeat, float3 start, float3 target)
{
	uint level = FindFieldLevel(start, target);
	if (level >= 3u)
		return 0.0;
	float voxel = u_GIField.VoxelOrigin[level].w;
	float3 delta = target - start;
	float distance = length(delta);
	uint steps = uint(ceil(distance / voxel));
	float transmittance = 1.0;
	[loop]
	for (uint step = 0u; step < steps; step++)
	{
		float3 position = start + delta * ((float(step) + 0.5) / float(steps));
		transmittance *= 1.0 - saturate(SampleFieldOccupancy(level, repeat, position) * ST_GI_OCCUPANCY_GAIN);
		if (transmittance < ST_GI_VISIBILITY_CUTOFF)
			return 0.0;
	}
	return transmittance;
}

// The voxel size of the finest of levels 0-2 holding a point (level 0's when none does).
float GetFieldVoxelSize(float3 position)
{
	[loop]
	for (uint level = 0u; level < 3u; level++)
	{
		if (IsInFieldLevel(level, position))
			return u_GIField.VoxelOrigin[level].w;
	}
	return max(u_GIField.VoxelOrigin[0].w, 1e-3);
}

// The field's irradiance over pi at a camera-relative surface point for a shading normal (luminance, times ST_GI_RADIANCE_SCALE),
// .a the field's weight there (0 outside it, while pending or where no voxel level holds the point). The eight cascade-0 probes around the point moved along its
// geometric normal, each weighted trilinearly, by its validity, by whether it lies in front of the surface's plane and by its
// visibility from just off the surface (a march through the voxels). Where no probe is visible, the march starts again moved off
// the solid (down the occupancy's gradient, a voxel at a time, at most two, while the start's occupancy reaches 0.3); a point that
// still sees none takes no field light (dark, never the sky). openShare receives the cosine-weighted share of the sky it sees.
float4 SampleIrradianceField(SamplerState repeat, float3 position, float3 geometricNormal, float3 shadingNormal, out float openShare)
{
	openShare = 1.0;
	float fieldWeight = GetFieldWeight(position);
	// A point no voxel level holds cannot check what it sees: today's light, as outside the field. The field's reach stays
	// where level 2 holds every point one scroll slab short (GlobalIllumination::ComputeLayout), so only a camera that moved on
	// by more than a step before the slab was voxelized meets this.
	if (fieldWeight <= 0.0 || FindFieldLevel(position, position) >= 3u)
		return float4(0.0, 0.0, 0.0, 0.0);
	float spacing = u_GIField.FieldOrigin.w;
	int3 grid = int3(u_GIField.FieldSize.xyz);
	float3 coordinate = (position + geometricNormal * (ST_GI_GATHER_NORMAL_BIAS * spacing) - u_GIField.FieldOrigin.xyz) / spacing - 0.5;
	int3 base = int3(floor(coordinate));
	float3 fraction = coordinate - float3(base);
	float voxel = GetFieldVoxelSize(position);
	float3 start = position + geometricNormal * (ST_GI_GATHER_START_VOXELS * voxel);

	float3 irradiance = float3(0.0, 0.0, 0.0);
	float sky = 0.0;
	float total = 0.0;
	[loop]
	for (uint pass = 0u; pass < 2u; pass++)
	{
		[loop]
		for (uint corner = 0u; corner < 8u; corner++)
		{
			int3 offset = int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
			int3 probe = base + offset;
			if (any(probe < int3(0, 0, 0)) || any(probe >= grid))
				continue;
			float validity = u_GIFieldValidity.Load(int4(probe, 0)).r;
			if (validity <= 0.0)
				continue;
			float3 probePosition = u_GIField.FieldOrigin.xyz + (float3(probe) + 0.5) * spacing;
			float3 toProbe = probePosition - position;
			float facing = smoothstep(-0.05, 0.25, dot(toProbe * rsqrt(max(dot(toProbe, toProbe), 1e-8)), geometricNormal));
			if (facing <= 0.0)
				continue;
			float3 trilinear3 = select(offset != int3(0, 0, 0), fraction, 1.0 - fraction);
			float weight = trilinear3.x * trilinear3.y * trilinear3.z * validity * facing;
			if (weight <= 0.0)
				continue;
			weight *= GetFieldVisibility(repeat, start, probePosition);
			if (weight <= 0.0)
				continue;
			float4 cube = ReadAmbientCube(probe, shadingNormal);
			irradiance += weight * cube.rgb;
			sky += weight * cube.a;
			total += weight;
		}
		if (total > 0.0 || pass == 1u)
			break;
		// No probe visible: the start may lie in the solid (a floor-wall junction); move it off down the occupancy's gradient.
		uint level = FindFieldLevel(start, start);
		if (level >= 3u)
			break;
		bool moved = false;
		[loop]
		for (uint step = 0u; step < 2u; step++)
		{
			if (SampleFieldOccupancy(level, repeat, start) < 0.3)
				break;
			float h = u_GIField.VoxelOrigin[level].w;
			float3 gradient = float3(
				SampleFieldOccupancy(level, repeat, start + float3(h, 0.0, 0.0)) - SampleFieldOccupancy(level, repeat, start - float3(h, 0.0, 0.0)),
				SampleFieldOccupancy(level, repeat, start + float3(0.0, h, 0.0)) - SampleFieldOccupancy(level, repeat, start - float3(0.0, h, 0.0)),
				SampleFieldOccupancy(level, repeat, start + float3(0.0, 0.0, h)) - SampleFieldOccupancy(level, repeat, start - float3(0.0, 0.0, h)));
			float gradientLength = length(gradient);
			if (gradientLength < 1e-4)
				break;
			start -= gradient / gradientLength * h;
			moved = true;
		}
		if (!moved)
			break;
	}
	openShare = total > 0.0 ? sky / total : 0.0;
	return float4(total > 0.0 ? irradiance / total : float3(0.0, 0.0, 0.0), fieldWeight);
}

#endif
