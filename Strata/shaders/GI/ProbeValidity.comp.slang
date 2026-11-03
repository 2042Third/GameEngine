#include "Include/Common.slang"

#include "Include/GIProbes.slang"

// The validity of one cascade's probes (Docs/DeveloperGuide.md, "Global illumination"): 1 - saturate(occupancy * gain) of the
// clipmap at the probe, from the cascade's minimum level up. 0: the probe is dead (inside a wall or a filled solid): its rays are
// not marched, and no gather or merge takes it. Continuous, so a probe a surface comes near fades rather than switching. Counts
// cascade 0's dead probes (counter 2).

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
		float occupancy = SampleOccupancy(GetProbePosition(globalID, u_Pass.Interval.x), u_Pass.MinimumLevel, finest);
		float validity = 1.0 - saturate(occupancy * ST_GI_OCCUPANCY_GAIN);
		o_Validity[globalID] = float4(validity, 0.0, 0.0, 0.0);
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
