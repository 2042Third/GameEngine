#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// One cascade's rays (Docs/DeveloperGuide.md, "Global illumination"), cascades top down: a thread per ray, the atlas texel
// (probe.xy * a + direction texel, probe.z). The direction is fixed (GetTileDirection: nothing jitters, nothing blends over
// frames). A dead probe writes zero. The ray marches the clipmap from the cascade's minimum level up, ST_GI_STEP_VOXELS voxels of
// the level sampled a step, over [start - lead-in, end + overlap), light front to back with transmittance. The lead-in reaches
// back to the probe for every cascade but the first and only absorbs: a wall between a child's interval and its parents' is
// crossed by a marched side (correction 2). Gathering light there took surfaces beside the parent that the child's line never
// meets (the sun room's ceiling probes read 2.5-4.8 times the reference through a parent 0.9 m above the floor's sunlit patch);
// the overlap (ProbeCascadeLayout::Overlap) takes what lies just past the interval's end on the child's own line instead. Within
// a run of occupied samples, every sample takes the light and normal read where the run began (less than a step past the last
// empty voxel: the layer the ray entered, never the one behind it, so a thin wall's sunlit outer face does not leak through its
// inner one).
// The miss rule: a ray that ends its interval unabsorbed is transparent and continues into the next cascade's merged rays (its
// eight parents by the probe's weights, each the mean of the four parent directions this one covers), never into the sky; no
// visible parent: dark. Only the top cascade, marching until it leaves every level, reads the sky. Output: rgb the merged light
// (scaled luminance), a the merged transmittance to the sky; cascade 0 (not the top) writes its own light and transmittance,
// unmerged: GI/FieldReduce.comp merges cascade 1 in each of its finer directions.

Texture3D<float4> u_Validity : register(t57);
Texture3D<float4> u_WeightsA : register(t58);
Texture3D<float4> u_WeightsB : register(t59);
Texture3D<float4> u_ParentAtlas : register(t60);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Atlas : register(u0);

float4 Merge(uint3 probe, uint2 texel)
{
	float4 weightsA = u_WeightsA.Load(int4(int3(probe), 0));
	float4 weightsB = u_WeightsB.Load(int4(int3(probe), 0));
	int3 base = int3(floor((float3(probe) - 0.5) * 0.5));
	uint parentDirections = u_Pass.ParentGrid.w;
	float4 sum = float4(0.0, 0.0, 0.0, 0.0);
	float total = 0.0;
	[loop]
	for (uint corner = 0u; corner < 8u; corner++)
	{
		float weight = corner < 4u ? weightsA[corner] : weightsB[corner - 4u];
		if (weight <= 0.0)
			continue;
		int3 parent = base + int3(int(corner & 1u), int((corner >> 1) & 1u), int((corner >> 2) & 1u));
		int2 first = parent.xy * int(parentDirections) + int2(texel) * 2;
		float4 rays = u_ParentAtlas.Load(int4(first, parent.z, 0)) + u_ParentAtlas.Load(int4(first + int2(1, 0), parent.z, 0))
			+ u_ParentAtlas.Load(int4(first + int2(0, 1), parent.z, 0)) + u_ParentAtlas.Load(int4(first + int2(1, 1), parent.z, 0));
		sum += weight * 0.25 * rays;
		total += weight;
	}
	return total < 1e-4 ? float4(0.0, 0.0, 0.0, 0.0) : sum / total;
}

[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID)
{
	uint a = u_Pass.DirectionsPerAxis;
	if (any(globalID >= uint3(u_Pass.Grid.xy * a, u_Pass.Grid.z)))
		return;
	uint3 probe = uint3(globalID.xy / a, globalID.z);
	uint2 texel = globalID.xy % a;
	if (u_Validity.Load(int4(int3(probe), 0)).r <= 0.0)
	{
		o_Atlas[globalID] = float4(0.0, 0.0, 0.0, 0.0);
		return;
	}

	float3 origin = GetProbePosition(probe, u_Pass.Interval.x);
	float3 direction = GetTileDirection(texel, a);
	float start = u_Pass.Interval.y;
	float end = u_Pass.Interval.z;
	bool top = end < 0.0;
	uint minimumLevel = u_Pass.MinimumLevel;

	float transmittance = 1.0;
	float3 light = float3(0.0, 0.0, 0.0);
	bool inRun = false;
	bool left = false;
	float3 runRadiance = float3(0.0, 0.0, 0.0);
	float t = max(0.0, start - u_Pass.Interval.w);
	[loop]
	for (uint step = 0u; step < ST_GI_MAX_MARCH_STEPS && (top || t < end); step++)
	{
		uint finest;
		float3 position = origin + direction * t;
		float occupancy = SampleOccupancy(position, minimumLevel, finest);
		if (finest == ST_GI_MAX_VOXEL_LEVELS)
		{
			left = true;
			break;
		}
		// The lead-in absorbs only solid voxels (correction 2): partial ones (alpha-masked, the layer in front of a face on the grid)
		// are taken once, in the interval.
		if (occupancy > 0.0 && (t >= start || occupancy >= ST_GI_LEAD_IN_OCCUPANCY))
		{
			if (!inRun)
				runRadiance = SampleSurfaceRadiance(position, minimumLevel, direction);
			inRun = true;
			float absorbed = saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
			if (t >= start)
				light += transmittance * absorbed * runRadiance;
			transmittance *= 1.0 - absorbed;
			if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
				break;
		}
		else
		{
			inRun = false;
		}
		t += ST_GI_STEP_VOXELS * u_GI.Levels[finest].Origin.w;
	}

	float4 rest = float4(0.0, 0.0, 0.0, 0.0);
	if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
		transmittance = 0.0;
	if (u_Pass.Grid.w == 0u && !top)
	{
		// Cascade 0 keeps its own light and transmittance: the field's reduce merges the next cascade per finer direction.
		o_Atlas[globalID] = float4(light, transmittance);
		return;
	}
	if (transmittance == 0.0)
		rest = float4(0.0, 0.0, 0.0, 0.0);
	else if (top)
		rest = left ? float4(GetSkyRadiance(direction), 1.0) : float4(0.0, 0.0, 0.0, 0.0);
	else
		rest = Merge(probe, texel);
	o_Atlas[globalID] = float4(light + transmittance * rest.rgb, transmittance * rest.a);
}
