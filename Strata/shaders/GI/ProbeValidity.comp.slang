#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// The validity of one cascade's probes (Docs/DeveloperGuide.md, "Global illumination"): 1 - saturate(occupancy * gain) of the
// clipmap at the probe, from the cascade's minimum level up. 0: the probe is dead (inside a wall or a filled solid): its rays are
// not marched, and no gather or merge takes it. Continuous, so a probe a surface comes near fades rather than switching. Counts
// cascade 0's dead probes (counter 2).
// A probe of cascades 1 and up that a surface covers moves off it along the surface's normal, by ST_GI_PROBE_MOVE_VOXELS voxels
// times the share the surface covers (continuous too), and takes the validity where it lands: probe planes lying on a room's
// faces would otherwise all die, and the children beside them take one surviving parent's view from metres away (the sun room's
// walls read three times their light on one side and a fifth on the other). Inside a solid no surface faces the probe (no
// normal): it stays dead. Cascade 0's probes stay where they are (the forward gather reads them at their grid positions). The
// move is stored in gba (EncodeProbeMove); the march and the weights start from the moved probe.

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
		if (u_Pass.Grid.w > 0u && occupancy > 0.0 && finest < ST_GI_MAX_VOXEL_LEVELS)
		{
			float3 normal = SampleSurfaceNormal(position, u_Pass.MinimumLevel);
			float normalLength = length(normal);
			if (normalLength > 1e-3)
			{
				float covered = saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
				float3 candidate = (normal / normalLength) * (ST_GI_PROBE_MOVE_VOXELS * u_GI.Levels[finest].Origin.w * covered);
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
