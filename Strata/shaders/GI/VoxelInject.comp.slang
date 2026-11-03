#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// Light injection (Docs/DeveloperGuide.md, "Global illumination"): every voxel of one level (u_Pass.Level), every frame, so moving
// the sun re-lights the clipmap in the frame it moved. A surface voxel (its normal's w 1, or 0.5 double-sided) sends
// albedo * E / pi + emission (luminance, times ST_GI_RADIANCE_SCALE), alpha 1: E the sun's illuminance on the voxel's normal
// times the transmittance of a march toward the sun through the clipmap from the voxel's own level up (from the surface's plane) (the shadow maps cover the
// view only; light reaches surfaces behind the camera too). A double-sided voxel turns its normal toward the sun. A two-sided
// voxel (w -1: both faces of a thin wall) sends nothing but stays a surface (alpha 1), so marches never take a neighbor's light
// for it. Voxels without a surface, and those outside the level's valid box, are zero. Counts the surface voxels (counter 4).

Texture3D<uint> u_GIEmission : register(t53);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Radiance : register(u0);
RWByteAddressBuffer o_Counters : register(u7);

groupshared uint s_SurfaceVoxels;

// The transmittance from a camera-relative point toward the sun (`direction`), one voxel of the level sampled a step.
float GetSunTransmittance(float3 start, float3 direction, uint level)
{
	float transmittance = 1.0;
	float t = 0.0;
	[loop]
	for (uint step = 0u; step < ST_GI_MAX_MARCH_STEPS; step++)
	{
		uint finest;
		float occupancy = SampleOccupancy(start + direction * t, level, finest);
		if (finest == ST_GI_MAX_VOXEL_LEVELS)
			break; // Out of the clipmap: the rest of the way is open
		transmittance *= 1.0 - saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
		if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
			return 0.0;
		t += u_GI.Levels[finest].Origin.w;
	}
	return transmittance;
}

[numthreads(4, 4, 4)]
void main(uint3 globalID : SV_DispatchThreadID, uint localIndex : SV_GroupIndex)
{
	if (localIndex == 0u)
		s_SurfaceVoxels = 0u;
	GroupMemoryBarrierWithGroupSync();

	uint3 dimensions = u_GI.Dimensions.xyz;
	uint level = u_Pass.Level;
	if (all(globalID < dimensions))
	{
		GIVoxelLevel data = u_GI.Levels[level];
		int3 texel = int3(globalID);
		int3 local = ((texel - data.Texel.xyz) % int3(dimensions) + int3(dimensions)) % int3(dimensions);
		bool valid = data.Texel.w != 0 && all(local >= data.ValidMinimum.xyz) && all(local < data.ValidMaximum.xyz);
		float4 normal = valid ? LoadGINormal(level, texel) : float4(0.0, 0.0, 0.0, 0.0);
		float4 radiance = float4(0.0, 0.0, 0.0, 0.0);
		if (normal.w < -0.5)
		{
			radiance.a = 1.0; // Two-sided: a dark surface
			InterlockedAdd(s_SurfaceVoxels, 1u);
		}
		else if (normal.w > 0.25)
		{
			InterlockedAdd(s_SurfaceVoxels, 1u);
			float3 albedo = LoadGIMaterial(level, texel).rgb;
			float3 light = UnpackRGB9E5(u_GIEmission.Load(int4(texel, 0)));
			if (u_Frame.DirectionalLightDirection.w > 0.0)
			{
				float3 toSun = -u_Frame.DirectionalLightDirection.xyz;
				float normalLength = length(normal.xyz);
				float3 n = normalLength > 1e-4 ? normal.xyz / normalLength : toSun;
				// Double-sided (w 0.5) voxels are lit from whichever side the sun is on.
				if (normal.w < 0.75 && dot(n, toSun) < 0.0)
					n = -n;
				float cosine = dot(n, toSun);
				if (cosine > 0.0)
				{
					float voxel = data.Origin.w;
					float3 center = data.Origin.xyz + (float3(local) + 0.5) * voxel;
					// From the surface's plane (half a voxel behind the center along the normal), the march starts where the ray
					// toward the sun is ST_GI_SHADOW_START_VOXELS voxels above that plane: clear of the surface's own voxels, and
					// on the ray through the surface (an offset along the normal would move the sunlit patch toward the window).
					float3 start = center - n * (0.5 * voxel) + toSun * (ST_GI_SHADOW_START_VOXELS * voxel / max(cosine, 0.25));
					float transmittance = GetSunTransmittance(start, toSun, level);
					light += albedo * u_Frame.DirectionalLightColor.rgb * (cosine * transmittance / PI);
				}
			}
			radiance = float4(min(SanitizeHDR(light) * ST_GI_RADIANCE_SCALE, float3(ST_GI_MAX_RADIANCE, ST_GI_MAX_RADIANCE, ST_GI_MAX_RADIANCE)), 1.0);
		}
		o_Radiance[globalID] = radiance;
	}

	GroupMemoryBarrierWithGroupSync();
	if (localIndex == 0u && s_SurfaceVoxels > 0u)
	{
		uint previous;
		o_Counters.InterlockedAdd(16u, s_SurfaceVoxels, previous);
	}
}
