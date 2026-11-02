#include "Include/Common.slang"

#include "Include/AtmospherePass.slang"

// The atmosphere's transmittance table (Include/Atmosphere.slang): from each height along each direction to the top of the
// atmosphere, ignoring the ground. Computed when the sky light's atmosphere settings change.

[vk::image_format("rgba16f")] RWTexture2D<float4> o_Table : register(u0);

void st_Main()
{
	int2 texel = int2(st_GlobalInvocationID.xy);
	if (any((texel >= int2(ST_TRANSMITTANCE_SIZE))))
		return;
	float2 radiusMu = TransmittanceUVToRadiusMu((float2(texel) + 0.5) / ST_TRANSMITTANCE_SIZE);
	o_Table[texel] = float4(ComputeTransmittance(GetMedium(), radiusMu.x, radiusMu.y), 1.0);
}

// --- Entry point ---
[numthreads(8, 8, 1)]
void main(uint3 globalID : SV_DispatchThreadID, uint3 localID : SV_GroupThreadID, uint3 groupID : SV_GroupID, uint localIndex : SV_GroupIndex)
{
	st_GlobalInvocationID = globalID;
	st_LocalInvocationID = localID;
	st_WorkGroupID = groupID;
	st_LocalInvocationIndex = localIndex;
	st_Main();
}
