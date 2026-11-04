#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// The weights with which a cascade's probes merge their eight trilinear parents of the next cascade (LAB probe_weights; Docs/
// DeveloperGuide.md, "Global illumination"): the trilinear weight (0.25 or 0.75 per axis: a parent's center is the center of its
// 2 x 2 x 2 children) times the parent's validity times the transmittance between the two probes (a soft march, one voxel of the
// level sampled a step): a wall between a child and a parent zeroes that parent. Parents outside the grid weigh 0. Stored once per
// probe (corners 0-3 and 4-7, corner c's offset (c & 1, c >> 1 & 1, c >> 2)); every ray of the probe shares them.

Texture3D<float4> u_Validity : register(t56);
Texture3D<float4> u_ParentValidity : register(t57);
[vk::image_format("rgba8")] RWTexture3D<float4> o_WeightsA : register(u0);
[vk::image_format("rgba8")] RWTexture3D<float4> o_WeightsB : register(u1);

float GetTransmittance(float3 from, float3 to, uint minimumLevel)
{
	float3 delta = to - from;
	float distance = length(delta);
	if (distance <= 0.0)
		return 1.0;
	float3 direction = delta / distance;
	float transmittance = 1.0;
	float t = 0.0;
	[loop]
	for (uint step = 0u; step < ST_GI_MAX_MARCH_STEPS && t < distance; step++)
	{
		uint finest;
		float occupancy = SampleOccupancy(from + direction * t, minimumLevel, finest);
		if (finest == ST_GI_MAX_VOXEL_LEVELS)
			break;
		transmittance *= 1.0 - saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
		if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
			return 0.0;
		t += u_GI.Levels[finest].Origin.w;
	}
	return transmittance;
}

[numthreads(4, 4, 4)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	if (any(globalID >= u_Pass.Grid.xyz))
		return;
	float spacing = u_Pass.Interval.x;
	// Between the probes as ProbeValidity.comp moved them off the surfaces they sat on.
	float3 child = GetProbePosition(globalID, spacing) + DecodeProbeMove(u_Validity.Load(int4(int3(globalID), 0)), spacing);
	// The child's center in the parent grid's probe coordinates: ((i + 0.5) s - 0.5 (2 s)) / (2 s).
	float3 parentCoordinate = (float3(globalID) - 0.5) * 0.5;
	int3 base = int3(floor(parentCoordinate));
	float3 fraction = parentCoordinate - float3(base);
	float weights[8];
	[loop]
	for (uint corner = 0u; corner < 8u; corner++)
	{
		int3 offset = int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		int3 parent = base + offset;
		weights[corner] = 0.0;
		if (any(parent < int3(0, 0, 0)) || any(parent >= int3(u_Pass.ParentGrid.xyz)))
			continue;
		float3 trilinear3 = select(offset != int3(0, 0, 0), fraction, 1.0 - fraction);
		float4 parentValidity = u_ParentValidity.Load(int4(parent, 0));
		float weight = trilinear3.x * trilinear3.y * trilinear3.z * parentValidity.r;
		if (weight <= 0.0)
			continue;
		float3 parentPosition = GetProbePosition(uint3(parent), 2.0 * spacing) + DecodeProbeMove(parentValidity, 2.0 * spacing);
		weights[corner] = weight * GetTransmittance(child, parentPosition, u_Pass.MinimumLevel);
	}
	o_WeightsA[globalID] = float4(weights[0], weights[1], weights[2], weights[3]);
	o_WeightsB[globalID] = float4(weights[4], weights[5], weights[6], weights[7]);
}
