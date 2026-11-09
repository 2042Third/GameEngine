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
// open space (relocation, below). The move is stored in gba (EncodeProbeMove); the march and the weights start from the moved
// probe.

[vk::image_format("rgba8")] RWTexture3D<float4> o_Validity : register(u0);
RWByteAddressBuffer o_Counters : register(u7);

groupshared uint s_Dead;

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
		if (occupancy > 0.0 && finest < ST_GI_MAX_VOXEL_LEVELS)
		{
			float3 normal = SampleSurfaceNormal(position, u_Pass.MinimumLevel);
			float normalLength = length(normal);
			if (normalLength > 1e-3)
			{
				float covered = saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
				// Cascade 0's probes move up to half a spacing: a wall on their plane beyond voxel level 0 lies in level 1's voxels,
				// twice as thick, and a shorter move left them inside it.
				float reach = u_Pass.Grid.w == 0u ? ST_GI_FIELD_PROBE_MOVE * spacing : ST_GI_PROBE_MOVE_VOXELS * u_GI.Levels[u_Pass.MinimumLevel].Origin.w;
				float3 candidate = (normal / normalLength) * (reach * covered);
				candidate = DecodeProbeMove(float4(0.0, EncodeProbeMove(candidate, spacing)), spacing); // As the passes will read it
				uint candidateFinest;
				float candidateOccupancy = SampleOccupancy(position + candidate, u_Pass.MinimumLevel, candidateFinest);
				if (candidateOccupancy < occupancy)
				{
					move = candidate;
					occupancy = candidateOccupancy;
				}
			}
		}
		// Probe relocation (D18.7): a cascade-0 probe still more than half inside a solid where it moved (in an object over a
		// contact, in a box beside a narrow gap) searches its cell for open space, so the gather there has a live probe beside
		// the surfaces instead of a dead one whose weight the probes around cannot make up: ST_GI_RELOCATION_RINGS rings of the
		// 26 directions, ST_GI_RELOCATION_STEP spacings apart per axis from twice that out to half a spacing, the lowest occupancy
		// plus ST_GI_RELOCATION_RING_COST a ring (the nearest open space of several). A probe no ring frees stays dead: deep in a
		// thick wall or a solid it says nothing about either side.
		[branch]
		if (u_Pass.Grid.w == 0u && occupancy * ST_GI_OCCUPANCY_GAIN > 0.5 && finest < ST_GI_MAX_VOXEL_LEVELS)
		{
			float bestScore = occupancy;
			float3 bestMove = move;
			// The outermost ring first: a probe it cannot free lies deeper than any ring reaches (most dead probes, deep in walls and
			// floors), and the inner rings are skipped.
			[loop]
			for (uint pass = 0u; pass < ST_GI_RELOCATION_RINGS; pass++)
			{
				uint ring = ST_GI_RELOCATION_RINGS - pass;
				float reach = ST_GI_RELOCATION_STEP * float(ring + 1u) * spacing;
				[loop]
				for (uint direction = 0u; direction < 27u; direction++)
				{
					int3 offset = int3(int(direction % 3u) - 1, int((direction / 3u) % 3u) - 1, int(direction / 9u) - 1);
					if (all(offset == int3(0, 0, 0)))
						continue;
					// As the passes will read it.
					float3 candidate = DecodeProbeMove(float4(0.0, EncodeProbeMove(float3(offset) * reach, spacing)), spacing);
					uint candidateFinest;
					float score = SampleOccupancy(position + candidate, u_Pass.MinimumLevel, candidateFinest) + ST_GI_RELOCATION_RING_COST * float(ring);
					if (score < bestScore)
					{
						bestScore = score;
						bestMove = candidate;
					}
				}
			}
			uint bestFinest;
			float bestOccupancy = SampleOccupancy(position + bestMove, u_Pass.MinimumLevel, bestFinest);
			if (bestOccupancy < occupancy)
			{
				move = bestMove;
				occupancy = bestOccupancy;
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
