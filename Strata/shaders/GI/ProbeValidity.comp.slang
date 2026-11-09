#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// The validity of one cascade's probes (Docs/DeveloperGuide.md, "Global illumination"): 1 - saturate(occupancy * gain) of the
// clipmap at the probe, from the cascade's minimum level up. 0: the probe is dead (inside a wall or a filled solid): its rays are
// not marched, and no gather or merge takes it. Continuous, so a probe a surface comes near fades rather than switching. Counts
// cascade 0's dead probes (counter 2).
// A probe that a surface covers moves off it along the surface's normal, by ST_GI_PROBE_MOVE_VOXELS voxels (cascade 0's by
// ST_GI_FIELD_PROBE_MOVE spacings)
// of the cascade's minimum level (the same from every camera: voxels of the finest level holding the probe doubled the move
// as the camera went away, and the probe's light stepped) times the share the surface covers (continuous too), and takes the
// validity where it lands: probe planes lying on a room's
// faces would otherwise all die, and the children beside them take one surviving parent's view from metres away (the sun room's
// walls read three times their light on one side and a fifth on the other). Inside a solid no surface faces the probe (no
// normal): it stays dead. Cascade 0's probes move too (probe relocation, DDGI's lesson): a room whose walls lie on a probe plane
// keeps live probes beside them, and the forward gather reads a moved probe's light and visibility from where it moved (its
// trilinear weight from its grid position). Cascade 0's probes that stay more than half inside a solid search their cell for
// open space (relocation, FindLevelProbeMove). The move is stored in gba (EncodeProbeMove); the march and the weights start from
// the moved probe.

[vk::image_format("rgba8")] RWTexture3D<float4> o_Validity : register(u0);
RWByteAddressBuffer o_Counters : register(u7);

groupshared uint s_Dead;

// A move as the passes will read it (EncodeProbeMove's rounding).
float3 QuantizeProbeMove(float3 move, float spacing)
{
	return DecodeProbeMove(float4(0.0, EncodeProbeMove(move, spacing)), spacing);
}

// A cascade-0 probe's move by one voxel level's voxels alone (world data: the same from every camera). Off the surface covering it
// along its normal by up to half a spacing times the share it covers (a wall on a probe plane beyond voxel level 0 lies in level
// 1's voxels, twice as thick, and a shorter move left the probe inside it). A probe still more than half inside a solid there (in
// an object over a contact, in a box beside a narrow gap) is relocated (D18.7): it searches its cell for open space, so the
// gather there has a live probe beside the surfaces instead of a dead one whose weight the probes around cannot make up:
// ST_GI_RELOCATION_RINGS rings of the 26 directions, ST_GI_RELOCATION_STEP spacings apart per axis from twice that out to half a
// spacing, the lowest occupancy plus ST_GI_RELOCATION_RING_COST a ring (the nearest open space of several). A probe no ring frees
// keeps its move: deep in a thick wall or a solid it says nothing about either side. Every read lies within half a spacing of the
// probe on every axis.
float3 FindLevelProbeMove(uint level, float3 position, float spacing)
{
	float occupancy = SampleLevelOccupancy(level, position);
	float3 move = float3(0.0, 0.0, 0.0);
	[branch]
	if (occupancy > 0.0)
	{
		float3 normal = SampleLevelSurfaceNormal(level, position);
		float normalLength = length(normal);
		[branch]
		if (normalLength > 1e-3)
		{
			float3 candidate = QuantizeProbeMove((normal / normalLength) * (ST_GI_FIELD_PROBE_MOVE * spacing * saturate(occupancy * ST_GI_OCCUPANCY_GAIN)), spacing);
			float candidateOccupancy = SampleLevelOccupancy(level, position + candidate);
			if (candidateOccupancy < occupancy)
			{
				move = candidate;
				occupancy = candidateOccupancy;
			}
		}
	}
	// The rings searched: none for a probe less than half inside a solid.
	uint rings = occupancy * ST_GI_OCCUPANCY_GAIN > 0.5 ? ST_GI_RELOCATION_RINGS : 0u;
	float bestScore = occupancy;
	// Rings from the outermost in; of equal scores the first found stays.
	[loop]
	for (uint pass = 0u; pass < rings; pass++)
	{
		uint ring = ST_GI_RELOCATION_RINGS - pass;
		float reach = ST_GI_RELOCATION_STEP * float(ring + 1u) * spacing;
		[loop]
		for (uint direction = 0u; direction < 27u; direction++)
		{
			int3 offset = int3(int(direction % 3u) - 1, int((direction / 3u) % 3u) - 1, int(direction / 9u) - 1);
			if (all(offset == int3(0, 0, 0)))
				continue;
			float3 candidate = QuantizeProbeMove(float3(offset) * reach, spacing);
			float score = SampleLevelOccupancy(level, position + candidate) + ST_GI_RELOCATION_RING_COST * float(ring);
			if (score < bestScore)
			{
				bestScore = score;
				move = candidate;
			}
		}
	}
	return move;
}

[numthreads(4, 4, 4)]
void main(uint3 globalID : SV_DispatchThreadID, uint localIndex : SV_GroupIndex)
{
	if (localIndex == 0u)
		s_Dead = 0u;
	GroupMemoryBarrierWithGroupSync();
	if (all(globalID < u_Pass.Grid.xyz))
	{
		uint finest;
		float spacing = u_Pass.Interval.x;
		float3 position = GetProbePosition(globalID, spacing);
		float occupancy = SampleOccupancy(position, u_Pass.MinimumLevel, finest);
		float3 move = float3(0.0, 0.0, 0.0);
		if (u_Pass.Grid.w == 0u)
		{
			// Each level's move from its own voxels, blended by the levels' weights moved inward by the moves' reach (half a spacing),
			// so every read a level's search takes lies in its valid box: a function of world data and continuous in the camera.
			// Searched in the occupancy blended by the camera's distance, the hard choices (whether to relocate, which candidate)
			// flipped as a probe crossed a blend (level 1's voxels make surfaces fatter), and it jumped up to 0.35 m in a frame.
			float reach = ST_GI_FIELD_PROBE_MOVE * spacing;
			// Unrolled: each copy reads its level's textures by a constant.
			float remaining = 1.0;
			[unroll]
			for (uint level = 0u; level < ST_GI_MAX_VOXEL_LEVELS; level++)
			{
				float weight = remaining > 0.0 && level >= u_Pass.MinimumLevel && level < u_GI.Dimensions.w ? GetLevelWeightWithin(level, position, reach) : 0.0;
				[branch]
				if (weight > 0.0)
				{
					move += remaining * weight * FindLevelProbeMove(level, position, spacing);
					remaining *= 1.0 - weight;
				}
			}
			move = QuantizeProbeMove(move, spacing);
			uint movedFinest;
			occupancy = SampleOccupancy(position + move, u_Pass.MinimumLevel, movedFinest);
		}
		else if (occupancy > 0.0 && finest < ST_GI_MAX_VOXEL_LEVELS)
		{
			float3 normal = SampleSurfaceNormal(position, u_Pass.MinimumLevel);
			float normalLength = length(normal);
			if (normalLength > 1e-3)
			{
				float covered = saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
				float reach = ST_GI_PROBE_MOVE_VOXELS * u_GI.Levels[u_Pass.MinimumLevel].Origin.w;
				float3 candidate = QuantizeProbeMove((normal / normalLength) * (reach * covered), spacing);
				uint candidateFinest;
				float candidateOccupancy = SampleOccupancy(position + candidate, u_Pass.MinimumLevel, candidateFinest);
				if (candidateOccupancy < occupancy)
				{
					move = candidate;
					occupancy = candidateOccupancy;
				}
			}
		}
		float validity = 1.0 - saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
		o_Validity[globalID] = float4(validity, EncodeProbeMove(move, spacing));
		if (validity <= 0.0)
			InterlockedAdd(s_Dead, 1u);
	}
	GroupMemoryBarrierWithGroupSync();
	if (localIndex == 0u && s_Dead > 0u && u_Pass.Grid.w == 0u)
	{
		uint previous;
		o_Counters.InterlockedAdd(8u, s_Dead, previous);
	}
}
