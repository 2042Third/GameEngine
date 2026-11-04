#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// Light injection (Docs/DeveloperGuide.md, "Global illumination"): every voxel of one level (u_Pass.Level), every frame, so moving
// the sun re-lights the clipmap in the frame it moved. A surface voxel (its normal's w 1, or 0.5 double-sided) sends
// albedo * E / pi + emission (luminance, times ST_GI_RADIANCE_SCALE), alpha 1: E the sun's illuminance on the voxel's normal
// times the transmittance of a walk toward the sun through the clipmap's voxels from the voxel's own level up, from the surface's
// plane (the shadow maps cover the view only; light reaches surfaces behind the camera too). A double-sided voxel turns its normal
// toward the sun. A two-sided voxel (w -1: both faces of a thin wall) sends nothing but stays a surface (alpha 1), so marches never
// take a neighbor's light for it. Voxels without a surface, and those outside the level's valid box, are zero. Counts the surface
// voxels (counter 4).

Texture3D<uint> u_GIEmission : register(t53);
[vk::image_format("rgba16f")] RWTexture3D<float4> o_Radiance : register(u0);
RWByteAddressBuffer o_Counters : register(u7);

groupshared uint s_SurfaceVoxels;

// Whether a camera-relative point lies in a level's valid box, less ST_GI_LEVEL_GUARD_VOXELS (the level holds its voxels).
bool IsInSunLevel(uint level, float3 position)
{
	GIVoxelLevel data = u_GI.Levels[level];
	if (data.Texel.w == 0)
		return false;
	float3 local = (position - data.Origin.xyz) / data.Origin.w;
	return all(local >= float3(data.ValidMinimum.xyz) + ST_GI_LEVEL_GUARD_VOXELS) && all(local <= float3(data.ValidMaximum.xyz) - ST_GI_LEVEL_GUARD_VOXELS);
}

// The level the sun's march reads at a camera-relative point: the voxel's own level (`level`) while the walk is still near
// its surface (`nearSurface`) and the level holds the point, whatever its weight there; otherwise the finest from `level` up
// whose weight there is at least one half (GetLevelWeight: the switch sits in the middle of the level blend and moves with the
// camera, never with the windows' scrolling); ST_GI_MAX_VOXEL_LEVELS where none holds it (the point left the clipmap). Near
// the surface a coarser level's voxel still holds the surface itself: a walk starting where its own level's weight is below
// one half read the floor's solid voxel of the next level and went dark (a ring at each level's edge, moving with the camera).
uint FindSunLevel(float3 position, uint level, bool nearSurface)
{
	if (nearSurface && IsInSunLevel(level, position))
		return level;
	[loop]
	for (uint candidate = level; candidate < u_GI.Dimensions.w; candidate++)
	{
		if (GetLevelWeight(candidate, position) >= 0.5)
			return candidate;
	}
	return ST_GI_MAX_VOXEL_LEVELS;
}

// The transmittance from a camera-relative point toward the sun (`direction`): a walk through the voxels the ray crosses, each read
// as it is stored. A trilinear march with the occupancy's gain thickens every occluder by a fraction of a voxel on each side and
// more at its edges (the sun room's 2 x 1.3 m window lit 48% of its patch's power that way). Solid voxels stop the light; voxels
// holding only a face on their boundary (ST_GI_TOUCH_OCCUPANCY: their solid neighbor occludes) pass it; partial ones (alpha-
// masked) attenuate it by their absorption over the share of a voxel crossed. The walk stays in the voxel's own level until it
// has risen ST_GI_SUN_OWN_LEVEL_VOXELS of its voxels off the start along the surface's normal `normal` (FindSunLevel): a coarser
// level's voxel no longer holds the surface there.
float GetSunTransmittance(float3 start, float3 direction, float3 normal, uint level)
{
	float ownRise = ST_GI_SUN_OWN_LEVEL_VOXELS * u_GI.Levels[level].Origin.w;
	float transmittance = 1.0;
	float3 position = start;
	// Axes the ray does not move along are never left.
	bool3 moving = abs(direction) > 1e-6;
	float3 inverse = select(moving, float3(1.0, 1.0, 1.0) / select(moving, direction, float3(1.0, 1.0, 1.0)), float3(0.0, 0.0, 0.0));
	float3 ahead = select(direction > float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0), float3(0.0, 0.0, 0.0));
	[loop]
	for (uint step = 0u; step < ST_GI_MAX_MARCH_STEPS; step++)
	{
		uint current = FindSunLevel(position, level, dot(position - start, normal) < ownRise);
		if (current == ST_GI_MAX_VOXEL_LEVELS)
			break; // Out of the clipmap: the rest of the way is open
		GIVoxelLevel data = u_GI.Levels[current];
		float voxel = data.Origin.w;
		float3 local = (position - data.Origin.xyz) / voxel;
		float3 cell = floor(local);
		// The distance (in voxels) to the cell's exit along the ray.
		float3 exits = select(moving, (cell + ahead - local) * inverse, float3(1e30, 1e30, 1e30));
		float exit = max(min(exits.x, min(exits.y, exits.z)), 0.0);
		float absorbed = saturate(LoadGIMaterial(current, GetVoxelTexel(current, int3(cell))).a * ST_GI_OCCUPANCY_GAIN);
		if (absorbed >= 1.0)
			return 0.0;
		if (absorbed > (ST_GI_TOUCH_OCCUPANCY + 0.5 / 255.0) * ST_GI_OCCUPANCY_GAIN)
		{
			transmittance *= exp2(log2(1.0 - absorbed) * exit);
			if (transmittance < ST_GI_TRANSMITTANCE_CUTOFF)
				return 0.0;
		}
		// Into the next cell (a thousandth of a voxel past the boundary, so the cell changes).
		position += direction * ((exit + 1e-3) * voxel);
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
					// A voxel holding only a face on its boundary (the layer in front of a face on the grid) is free: the walk starts on
					// the face, half a voxel behind the center (the shadow's edges stay where the geometry puts them). A solid one holds
					// the surface somewhere inside: the walk starts ST_GI_SHADOW_OFFSET_VOXELS along the normal from its center, off the
					// surface's own voxels, and never skips a solid voxel (the floor runs on under a wall: the wall's voxels at the
					// floor's height hold the floor's face, and skipping them let the sun into a sealed room).
					bool touch = LoadGIMaterial(level, texel).a <= ST_GI_TOUCH_OCCUPANCY + 0.5 / 255.0;
					float3 start = touch ? center - n * (0.49 * voxel) : center + n * (ST_GI_SHADOW_OFFSET_VOXELS * voxel);
					float transmittance = GetSunTransmittance(start, toSun, n, level);
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
