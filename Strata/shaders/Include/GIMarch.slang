#ifndef STRATA_GI_MARCH_SLANG
#define STRATA_GI_MARCH_SLANG

// A probe ray's march through global illumination's voxel clipmap (GI/CascadeMarch.comp, GI/FieldReduce.comp; Docs/DeveloperGuide.md,
// "Global illumination"). Needs the levels' radiance (the march's bindings).

#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// One ray's own light (rgb, scaled luminance) and transmittance (a) over [start - the pass's lead-in, end) (top: until it leaves
// every level, `left`), from `minimumLevel` up, ST_GI_STEP_VOXELS voxels of the level sampled a step, front to back. The lead-in
// absorbs only solid voxels (correction 2: partial ones, alpha-masked or the layer in front of a face on the grid, are taken once,
// in the interval) and gathers no light. Each run of occupied samples takes the light read at its samples up to
// ST_GI_REFRESH_OCCUPANCY full (the layer in front of the surface the ray entered; one read at the run's first sample and nothing
// deeper, so a thin wall's sunlit far face never lights its near one), each weighted by what the samples from it on absorb: a ray
// grazing a floor is absorbed where it reaches the floor's sunlit patch, not where it first touched the floor's layer before it.
// `hit` receives the distances at which the ray is absorbed past its origin: x the sum of each sample's absorbed share times its
// distance, y times its square, z the shares' sum (FieldReduce.comp's depth moments). The origin's own sample is the probe's
// validity, not a hit: a probe half in a wall's surface sees the room on its open side. What the cutoff leaves is in none.
float4 MarchRay(float3 origin, float3 direction, float start, float end, bool top, uint minimumLevel, out bool left, out float3 hit)
{
	float transmittance = 1.0;
	hit = float3(0.0, 0.0, 0.0);
	float hitTransmittance = 1.0;
	float3 light = float3(0.0, 0.0, 0.0);
	bool inRun = false;
	left = false;
	float3 runRadiance = float3(0.0, 0.0, 0.0);
	float runWeight = 0.0;
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
		if (occupancy > 0.0 && (t >= start || occupancy >= ST_GI_LEAD_IN_OCCUPANCY))
		{
			if (!inRun || occupancy <= ST_GI_REFRESH_OCCUPANCY)
			{
				light += runWeight * runRadiance;
				runWeight = 0.0;
				runRadiance = SampleSurfaceRadiance(position, minimumLevel, direction);
			}
			inRun = true;
			float absorbed = saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
			if (t >= start)
				runWeight += transmittance * absorbed;
			if (t > 0.0)
			{
				hit += hitTransmittance * absorbed * float3(t, t * t, 1.0);
				hitTransmittance *= 1.0 - absorbed;
			}
			transmittance *= 1.0 - absorbed;
			if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
				break;
		}
		else
		{
			light += runWeight * runRadiance;
			runWeight = 0.0;
			inRun = false;
		}
		t += ST_GI_STEP_VOXELS * u_GI.Levels[finest].Origin.w;
	}
	light += runWeight * runRadiance;
	if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
		transmittance = 0.0;
	return float4(light, transmittance);
}

#endif
